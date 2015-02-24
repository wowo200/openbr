/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2012 The MITRE Corporation                                      *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <openbr/plugins/openbr_internal.h>
#include <openbr/core/opencvutils.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

using namespace cv;

namespace br
{

class AVCapture
{

public:
    void init()
    {
        av_register_all();
        avformat_network_init();
        avFormatCtx = NULL;
        avCodecCtx = NULL;
        avSwsCtx = NULL;
        avCodec = NULL;
        frame = NULL;
        bgr_frame = NULL;
        buffer = NULL;
        opened = false;
        streamID = -1;
        fps = 0.f;
        time_base = 0.f;
        timeIdx = -1;
        filename = QString();
    }

    AVCapture()
    {
        init();
    }

    AVCapture(const QString filename)
    {
        init();
        opened = open(filename);
    }

    ~AVCapture()
    {
        release();
    }

    bool open(const QString url)
    {
        filename = url;
        if (opened) release();
        if (avformat_open_input(&avFormatCtx, filename.toStdString().c_str(), NULL, NULL) != 0) {
            qFatal("Failed to open %s for reading.", qPrintable(filename));
        } else if (avformat_find_stream_info(avFormatCtx, NULL) < 0) {
            qFatal("Failed to read stream info for %s.", qPrintable(filename));
        } else {
            for (unsigned int i=0; i<avFormatCtx->nb_streams; i++) {
                if (avFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                    streamID = i;
                    break;
                }
            }
        }

        if (streamID == -1)
            qFatal("Failed to find video stream for %s", qPrintable(filename));

        avCodecCtx = avFormatCtx->streams[streamID]->codec;
        avCodec = avcodec_find_decoder(avCodecCtx->codec_id);
        if (avCodec == NULL)
            qFatal("Unsupported codec for %s!", qPrintable(filename));

        if (avcodec_open2(avCodecCtx, avCodec, NULL) < 0)
            qFatal("Could not open codec for file %s", qPrintable(filename));

        frame = av_frame_alloc();
        bgr_frame = av_frame_alloc();

        // Get fps, stream time_base and allocate space for frame buffer with av_malloc.
        fps = (float)avFormatCtx->streams[streamID]->avg_frame_rate.num /
              (float)avFormatCtx->streams[streamID]->avg_frame_rate.den;
        time_base = (float)avFormatCtx->streams[streamID]->time_base.num /
                    (float)avFormatCtx->streams[streamID]->time_base.den;
        int framebytes = avpicture_get_size(AV_PIX_FMT_BGR24, avCodecCtx->width, avCodecCtx->height);
        buffer = (uint8_t*)av_malloc(framebytes*sizeof(uint8_t));
        avpicture_fill((AVPicture*)bgr_frame, buffer, AV_PIX_FMT_BGR24, avCodecCtx->width, avCodecCtx->height);

        avSwsCtx = sws_getContext(avCodecCtx->width, avCodecCtx->height,
                                  avCodecCtx->pix_fmt,
                                  avCodecCtx->width, avCodecCtx->height,
                                  AV_PIX_FMT_BGR24,
                                  SWS_BICUBIC,
                                  NULL, NULL, NULL);

        // attempt to seek to first keyframe
        if (av_seek_frame(avFormatCtx, streamID, avFormatCtx->streams[streamID]->start_time, 0) < 0)
            qFatal("Could not seek to beginning keyframe for %s!", qPrintable(filename));
        avcodec_flush_buffers(avCodecCtx);
        opened = true;
        return opened;
    }

    bool isOpened() const
    {
        return opened;
    }

    float getFPS() const
    {
        return fps;
    }

    int getCurrentFrame() const
    {
        return (timeIdx * time_base) * fps;
    }

    // in seconds
    int getCurrentTime() const
    {
        return timeIdx * time_base;
    }
    void release()
    {
        if (avSwsCtx)     sws_freeContext(avSwsCtx);
        if (frame)        av_free(frame);
        if (bgr_frame)    av_free(bgr_frame);
        if (avCodecCtx)   avcodec_close(avCodecCtx);
        if (avFormatCtx)  avformat_close_input(&avFormatCtx);
        if (buffer)       av_free(buffer);
    }

    // returns frame number, or -1 on failure to read frame
    int read(Mat &image)
    {
        AVPacket packet;
        av_init_packet(&packet);
        int ret = 0;
        while (!ret) {
            if (av_read_frame(avFormatCtx, &packet) >= 0) {
                if (packet.stream_index == streamID) {
                    avcodec_decode_video2(avCodecCtx, frame, &ret, &packet);
                    timeIdx = packet.dts; // decompression timestamp
                    av_free_packet(&packet);
                } else {
                    av_free_packet(&packet);
                }
            } else {
                av_free_packet(&packet);
                opened = false;
                return -1;
            }
        }

        // Convert from native format to BGR
        sws_scale(avSwsCtx,
                  frame->data,
                  frame->linesize,
                  0, avCodecCtx->height,
                  bgr_frame->data,
                  bgr_frame->linesize);

        image = Mat(avCodecCtx->height, avCodecCtx->width, CV_8UC3, bgr_frame->data[0]);
        if (image.data) {
            if (av_seek_frame(avFormatCtx, streamID, timeIdx+1, 0) < 0)
                opened = false;
            avcodec_flush_buffers(avCodecCtx);
            return (timeIdx * time_base) * fps;
        }
        opened = false;
        return -1;
    }

private:
    AVFormatContext *avFormatCtx;
    AVCodecContext *avCodecCtx;
    SwsContext *avSwsCtx;
    AVCodec *avCodec;
    AVFrame *frame;
    AVFrame *bgr_frame;
    uint8_t *buffer;
    bool opened;
    int streamID;
    float fps;
    float time_base;
    int64_t timeIdx;
    QString filename;
};

/*!
 * \ingroup transforms
 * \brief Read key frames of a video with LibAV
 * \author Ben Klein \cite bhklein
 */
class ReadKeyframesTransform : public UntrainableMetaTransform
{
    Q_OBJECT
private:

    void project(const Template &src, Template &dst) const
    {
        (void) src; (void) dst;
        qFatal("ReadVideoTransform does not support single template project!");
    }

    void project(const TemplateList &srcList, TemplateList &dstList) const
    {
        foreach(const Template &src, srcList) {
            AVCapture cap(src.file.name);
            bool opened = cap.isOpened();
            QString URL = src.file.get<QString>("URL", "");
            float fps = cap.getFPS();
            int currentFrame = 0;
            while (opened) {
                Mat temp;
                currentFrame = cap.read(temp);
                opened = cap.isOpened();
                if (temp.data) {
                    Template dst(temp.clone());
                    dst.file = src.file;
                    dst.file.set("URL", URL + "#t=" + QString::number((int)(currentFrame/fps)) + "s");
                    dst.file.set("FrameNumber", QString::number(currentFrame));
                    dstList.append(dst);
                } else {
                    qDebug("Frame empty!");
                }
            }
        }
    }
};

BR_REGISTER(Transform, ReadKeyframesTransform)

} // namespace br

#include "video/readkeyframes.moc"
