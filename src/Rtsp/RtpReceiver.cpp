/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Common/config.h"
#include "RtpReceiver.h"

#define AV_RB16(x)                           \
    ((((const uint8_t*)(x))[0] << 8) |          \
      ((const uint8_t*)(x))[1])

#define RTP_MAX_SIZE (10 * 1024)

namespace mediakit {

RtpReceiver::RtpReceiver() {
    int index = 0;
    for (auto &sortor : _rtp_sortor) {
        sortor.setOnSort([this, index](uint16_t seq, RtpPacket::Ptr &packet) {
            onRtpSorted(packet, index);
        });
        ++index;
    }
}
RtpReceiver::~RtpReceiver() {}

bool RtpReceiver::handleOneRtp(int track_index, TrackType type, int samplerate, unsigned char *rtp_raw_ptr, unsigned int rtp_raw_len) {
    if (rtp_raw_len < 12) {
        WarnL << "rtp包太小:" << rtp_raw_len;
        return false;
    }

    //发现jt1078流媒体的包头
    if (rtp_raw_ptr[0] == 0x30 && rtp_raw_ptr[1] == 0x31 
        && rtp_raw_ptr[2] == 0x63 && rtp_raw_ptr[3] == 0x64) {
        return handleJT1078Rtp(track_index, type, samplerate, rtp_raw_ptr,rtp_raw_len);
    }
    uint32_t version = rtp_raw_ptr[0] >> 6;
    uint8_t padding = 0;
    uint8_t ext = rtp_raw_ptr[0] & 0x10;
    uint8_t csrc = rtp_raw_ptr[0] & 0x0f;

    if (rtp_raw_ptr[0] & 0x20) {
        //获取padding大小
        padding = rtp_raw_ptr[rtp_raw_len - 1];
        //移除padding flag
        rtp_raw_ptr[0] &= ~0x20;
        //移除padding字节
        rtp_raw_len -= padding;
    }

    if (version != 2) {
        throw std::invalid_argument("非法的rtp，version != 2");
    }

    auto rtp_ptr = _rtp_pool.obtain();
    auto &rtp = *rtp_ptr;

    rtp.type = type;
    rtp.interleaved = 2 * type;
    rtp.mark = rtp_raw_ptr[1] >> 7;
    rtp.PT = rtp_raw_ptr[1] & 0x7F;

    //序列号,内存对齐
    memcpy(&rtp.sequence, rtp_raw_ptr + 2, 2);
    rtp.sequence = ntohs(rtp.sequence);

    //时间戳,内存对齐
    memcpy(&rtp.timeStamp, rtp_raw_ptr + 4, 4);
    rtp.timeStamp = ntohl(rtp.timeStamp);

    if (!samplerate) {
        //无法把时间戳转换成毫秒
        return false;
    }
    //时间戳转换成毫秒
    rtp.timeStamp = rtp.timeStamp * 1000LL / samplerate;

    //ssrc,内存对齐
    memcpy(&rtp.ssrc, rtp_raw_ptr + 8, 4);
    rtp.ssrc = ntohl(rtp.ssrc);

    if (_ssrc[track_index] != rtp.ssrc) {
        if (_ssrc[track_index] == 0) {
            //保存SSRC至track对象
            _ssrc[track_index] = rtp.ssrc;
        } else {
            //ssrc错误
            WarnL << "ssrc错误:" << rtp.ssrc << " != " << _ssrc[track_index];
            if (_ssrc_err_count[track_index]++ > 10) {
                //ssrc切换后清除老数据
                WarnL << "ssrc更换:" << _ssrc[track_index] << " -> " << rtp.ssrc;
                _rtp_sortor[track_index].clear();
                _ssrc[track_index] = rtp.ssrc;
            }
            return false;
        }
    }

    //ssrc匹配正确，不匹配计数清零
    _ssrc_err_count[track_index] = 0;

    //获取rtp中媒体数据偏移量
    rtp.offset = 12 + 4;
    rtp.offset += 4 * csrc;
    if (ext && rtp_raw_len >= rtp.offset) {
        /* calculate the header extension length (stored as number of 32-bit words) */
        ext = (AV_RB16(rtp_raw_ptr + rtp.offset - 2) + 1) << 2;
        rtp.offset += ext;
    }

    if (rtp_raw_len + 4 <= rtp.offset) {
        WarnL << "无有效负载的rtp包:" << rtp_raw_len << " <= " << (int) rtp.offset;
        return false;
    }

    if (rtp_raw_len > RTP_MAX_SIZE) {
        WarnL << "超大的rtp包:" << rtp_raw_len << " > " << RTP_MAX_SIZE;
        return false;
    }

    //设置rtp负载长度
    rtp.setCapacity(rtp_raw_len + 4);
    rtp.setSize(rtp_raw_len + 4);
    uint8_t *payload_ptr = (uint8_t *) rtp.data();
    payload_ptr[0] = '$';
    payload_ptr[1] = rtp.interleaved;
    payload_ptr[2] = rtp_raw_len >> 8;
    payload_ptr[3] = (rtp_raw_len & 0x00FF);
    //拷贝rtp负载
    memcpy(payload_ptr + 4, rtp_raw_ptr, rtp_raw_len);

    //排序rtp
    auto seq = rtp_ptr->sequence;
    _rtp_sortor[track_index].sortPacket(seq, std::move(rtp_ptr));
    return true;
}

std::string RtpReceiver::bcdCode(const char* data, int len)
{
    std::stringstream ss;
    char szAscII[] = "0123456789abcdef";
    for (int i = 0; i < len; i++)
    {
        int b;
        b = 0x0f & (data[i] >> 4);
        ss << szAscII[b];
        b = 0x0f & data[i];
        ss << szAscII[b];
    }
    return ss.str();
}

bool RtpReceiver::handleJT1078Rtp(int track_index, TrackType type, int samplerate, unsigned char* rtp_raw_ptr, unsigned int rtp_raw_len) {
    //version & padding & ext & csrc
    uint32_t version = rtp_raw_ptr[4] >> 6;
    uint8_t padding = rtp_raw_ptr[4] >> 5 & 0&01;
    uint8_t ext = rtp_raw_ptr[4] & 0x10;
    uint8_t csrc = rtp_raw_ptr[4] & 0x0f;
    if (!(version == 2 && padding == 0 && ext == 0 && csrc == 1)) {
        //非jt1078忽略这个包
        return false;
    }
    //SIM卡
    string simcard = bcdCode((const char*)rtp_raw_ptr + 8, 6);
    //通道号
    uint8_t channelId = rtp_raw_ptr[14];
    //数据类型
    uint8_t data_type = rtp_raw_ptr[15] >> 4;
    //分包类型
    uint8_t packetType = rtp_raw_ptr[15] & 0xf;
    //忽略透传数据
    if (data_type == 0b0100) {
        return false;
    }
    //读取数据体长度
    uint8_t length_offset = 28;
    if (data_type == 0b0011) {
        //音频没有LastIFrameInterval和LastFrameInterval,所以去掉4个字节的偏移
        length_offset -= 4;
    }
    uint16_t packet_length = 0;
    memcpy(&packet_length, rtp_raw_ptr + length_offset, 2);
    packet_length = ntohs(packet_length);
    //组装rtp并排序
    auto rtp_ptr = _rtp_pool.obtain();
    auto& rtp = *rtp_ptr;

    rtp.type = type;
    rtp.interleaved = 2 * type;
    rtp.mark = rtp_raw_ptr[5] >> 7;
    rtp.PT = rtp_raw_ptr[5] & 0x7F;

    //序列号,内存对齐
    memcpy(&rtp.sequence, rtp_raw_ptr + 6, 2);
    rtp.sequence = ntohs(rtp.sequence);

    //时间戳,内存对齐,1078的时间戳是8byte
    uint64_t timeStamp = 0;
    memcpy(&timeStamp, rtp_raw_ptr + 16, 8);
    timeStamp = ntohll(timeStamp);
    rtp.timeStamp = timeStamp % 1000000000LL;

    if (!samplerate) {
        //无法把时间戳转换成毫秒
        return false;
    }
    //时间戳转换成毫秒
    rtp.timeStamp = rtp.timeStamp * 1000LL / samplerate;

    //将sim卡当作ssrc,但是sim卡长度超过uint32_t，将将第一位去掉（通常为1）,内存对齐
    auto x = _atoi64(simcard.c_str()) % 1000000000LL;
    rtp.ssrc = x;
    if (_ssrc[track_index] != rtp.ssrc) {
        if (_ssrc[track_index] == 0) {
            //保存SSRC至track对象
            _ssrc[track_index] = rtp.ssrc;
        }
        else {
            //ssrc错误
            WarnL << "ssrc错误:" << rtp.ssrc << " != " << _ssrc[track_index];
            if (_ssrc_err_count[track_index]++ > 10) {
                //ssrc切换后清除老数据
                WarnL << "ssrc更换:" << _ssrc[track_index] << " -> " << rtp.ssrc;
                _rtp_sortor[track_index].clear();
                _ssrc[track_index] = rtp.ssrc;
            }
            return false;
        }
    }

    //ssrc匹配正确，不匹配计数清零
    _ssrc_err_count[track_index] = 0;

    //获取rtp中媒体数据偏移量
    if (rtp_raw_len <= length_offset + 2) {
        WarnL << "无有效负载的rtp包:" << rtp_raw_len << " <= " << length_offset + 2;
        return false;
    }

    if (rtp_raw_len > RTP_MAX_SIZE) {
        WarnL << "超大的rtp包:" << rtp_raw_len << " > " << RTP_MAX_SIZE;
        return false;
    }

    //设置rtp负载长度-4是因为1078的rtp中包含了0x00000001分隔符,在此跳过
    rtp.setCapacity(packet_length + 4 - 4);
    rtp.setSize(packet_length + 4 - 4);
    uint8_t* payload_ptr = (uint8_t*)rtp.data();
    payload_ptr[0] = '$';
    payload_ptr[1] = rtp.interleaved;
    payload_ptr[2] = packet_length >> 8;
    payload_ptr[3] = (packet_length & 0x00FF);
    //拷贝rtp负载
    memcpy(payload_ptr + 4, rtp_raw_ptr + length_offset + 2 + 4, packet_length - 4);

    //排序rtp
    auto seq = rtp_ptr->sequence;
    _rtp_sortor[track_index].sortPacket(seq, std::move(rtp_ptr));
    return true;
}

void RtpReceiver::clear() {
    CLEAR_ARR(_ssrc);
    CLEAR_ARR(_ssrc_err_count);
    for (auto &sortor : _rtp_sortor) {
        sortor.clear();
    }
}

void RtpReceiver::setPoolSize(int size) {
    _rtp_pool.setSize(size);
}

int RtpReceiver::getJitterSize(int track_index){
    return _rtp_sortor[track_index].getJitterSize();
}

int RtpReceiver::getCycleCount(int track_index){
    return _rtp_sortor[track_index].getCycleCount();
}


}//namespace mediakit
