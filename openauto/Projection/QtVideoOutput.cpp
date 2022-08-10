/*
*  This file is part of openauto project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  openauto is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  openauto is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with openauto. If not, see <http://www.gnu.org/licenses/>.
*/

#include <QApplication>
#include "openauto/Projection/QtVideoOutput.hpp"
#include "OpenautoLog.hpp"

#include <QLabel>
#include <QImage>
#include <QScrollArea>

namespace openauto
{
namespace projection
{

QtVideoOutput::QtVideoOutput(configuration::IConfiguration::Pointer configuration, QWidget* videoContainer)
    : VideoOutput(std::move(configuration))
    , videoContainer_(videoContainer)
{
    this->moveToThread(QApplication::instance()->thread());
    connect(this, &QtVideoOutput::startPlayback, this, &QtVideoOutput::onStartPlayback, Qt::QueuedConnection);
    connect(this, &QtVideoOutput::stopPlayback, this, &QtVideoOutput::onStopPlayback, Qt::QueuedConnection);

    QMetaObject::invokeMethod(this, "createVideoOutput", Qt::BlockingQueuedConnection);
}

void QtVideoOutput::createVideoOutput()
{
    OPENAUTO_LOG(debug) << "[QtVideoOutput] create.";
    scrollArea_ = new QScrollArea;//creating a scroll area
    scrollArea_->setBackgroundRole(QPalette::Dark);
    scrollArea_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    imageLabel_ = new QLabel;//creating a new label for display
    imageLabel_->setBackgroundRole(QPalette::Base);
    imageLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    imageLabel_->setScaledContents(true);

    scrollArea_->setWidget(imageLabel_);
}


bool QtVideoOutput::open()
{
    //printf("Test Open\n");
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Packet not found\n");
        exit(1);
    }

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "parser not found\n");
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }
    return true;
    //return videoBuffer_.open(QIODevice::ReadWrite);
}

bool QtVideoOutput::init()
{
    emit startPlayback();
    return true;
}

void QtVideoOutput::stop()
{
    emit stopPlayback();
}

void QtVideoOutput::write(uint64_t, const aasdk::common::DataConstBuffer& buffer)
{
    data = buffer.cdata;
    data_size = buffer.size;
    while (data_size > 0) {
        printf("Receiving %d bytes\n", data_size);
        ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                               data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            fprintf(stderr, "Error while parsing\n");
            exit(1);
        }
        data      += ret;
        data_size -= ret;

        if (pkt->size){
            decode(c, frame, pkt);
        }
    }
}

void QtVideoOutput::resize()
{
    if(scrollArea_ != nullptr && videoContainer_ != nullptr)
    {
        scrollArea_->resize(videoContainer_->size());
    }
}

void QtVideoOutput::onStartPlayback()
{
    if(videoContainer_ == nullptr)
    {
        scrollArea_->setFocus();
        scrollArea_->setWindowFlags(Qt::WindowStaysOnTopHint);
        scrollArea_->resize(900,500);
    }
    else
    {
        scrollArea_->resize(videoContainer_->size());
    }
    scrollArea_->show();
}

void QtVideoOutput::onStopPlayback()
{
    scrollArea_->hide();
}

void QtVideoOutput::decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt){
    int ret;
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    while (ret >= 0) {

        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            return;
        }
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        if(dec_ctx->frame_number){
            QImage image = getQImageFromFrame(frame);
            printf("saving frame %3d\n", dec_ctx->frame_number);
            imageLabel_->adjustSize();
            imageLabel_->setPixmap(QPixmap::fromImage(image));
            //sleep(1/30);
        }

        fflush(stdout);

    }
}

QImage QtVideoOutput::getQImageFromFrame(const AVFrame* pFrame)
{
    // first convert the input AVFrame to the desired format

    SwsContext* img_convert_ctx = sws_getContext(
                                     pFrame->width,
                                     pFrame->height,
                                     (AVPixelFormat)pFrame->format,
                                     pFrame->width,
                                     pFrame->height,
                                     AV_PIX_FMT_RGB24,
                                     SWS_BICUBIC, NULL, NULL, NULL);
    if(!img_convert_ctx){
        qDebug("Failed to create sws context");
        return QImage();
    }

    // prepare line sizes structure as sws_scale expects
    int rgb_linesizes[8] = {0};
    rgb_linesizes[0] = 3*pFrame->width;

    // prepare char buffer in array, as sws_scale expects
    unsigned char* rgbData[8];
    int imgBytesSyze = 3*pFrame->height*pFrame->width;
    // as explained above, we need to alloc extra 64 bytes
    rgbData[0] = (unsigned char *)malloc(imgBytesSyze+64);
    if(!rgbData[0]){
        qDebug("Error allocating buffer for frame conversion");
        free(rgbData[0]);
        sws_freeContext(img_convert_ctx);
        return QImage();
    }
    if(sws_scale(img_convert_ctx,
                pFrame->data,
                pFrame->linesize, 0,
                pFrame->height,
                rgbData,
                rgb_linesizes)
            != pFrame->height){
        qDebug("Error changing frame color range");
        free(rgbData[0]);
        sws_freeContext(img_convert_ctx);
        return QImage();
    }

    // then create QImage and copy converted frame data into it

    QImage image(pFrame->width,
                 pFrame->height,
                 QImage::Format_RGB888);

    for(int y=0; y<pFrame->height; y++){
        memcpy(image.scanLine(y), rgbData[0]+y*3*pFrame->width, 3*pFrame->width);
    }

    free(rgbData[0]);
    sws_freeContext(img_convert_ctx);
    return image;
}


}
}
