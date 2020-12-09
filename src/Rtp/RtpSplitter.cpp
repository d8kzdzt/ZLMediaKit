/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_RTPPROXY)
#include <string.h>
#include "RtpSplitter.h"
namespace mediakit{

static const char kEHOME_MAGIC[] = "\x01\x00\x01\x00";
static const char kJT1078_MAGIC[] = "\x30\x31\x63\x64";
static const int  kEHOME_OFFSET = 256;

RtpSplitter::RtpSplitter() {}
RtpSplitter::~RtpSplitter() {}

int64_t RtpSplitter::onRecvHeader(const char *data,uint64_t len){
    //忽略偏移量
    data += _offset;
    len -= _offset;

    if (_is_ehome && len > 12 && data[12] == '\r') {
        //这是ehome,移除第12个字节
        memmove((char *) data + 1, data, 12);
        data += 1;
        len -= 1;
    }
    onRtpPacket(data, len);
    return 0;
}

static bool isEhome(const char *data, uint64_t len){
    if (len < 4) {
        return false;
    }
    return memcmp(data, kEHOME_MAGIC, sizeof(kEHOME_MAGIC) - 1) == 0;
}
static bool isJT1078(const char* data, uint64_t len) {
    if (len < 4) {
        return false;
    }
    bool _1078 = memcmp(data, kJT1078_MAGIC, sizeof(kJT1078_MAGIC) - 1) == 0;
    return _1078;
}

const char *RtpSplitter::onSearchPacketTail(const char *data, uint64_t len) {
    if (len < 4) {
        //数据不够
        return nullptr;
    }

    if (isEhome(data, len)) {
        //是ehome协议
        if (len < kEHOME_OFFSET + 4) {
            //数据不够
            return nullptr;
        }
        //忽略ehome私有头后是rtsp样式的rtp，多4个字节，
        _offset = kEHOME_OFFSET + 4;
        _is_ehome = true;
        //忽略ehome私有头
        return onSearchPacketTail_l(data + kEHOME_OFFSET + 2, len - kEHOME_OFFSET - 2);
    }

    if (data[0] == '$') {
        //可能是4个字节的rtp头
        _offset = 4;
        return onSearchPacketTail_l(data + 2, len - 2);
    }        
    if (isJT1078(data, len)) {
        //是1078协议
        if (len < 26) {
            //数据不够
            return nullptr;
        }
        _offset = 0;
        _is_jt1078 = true;
        return onSearchPacketTail_l(data, len);
    }
    //两个字节的rtp头
    _offset = 2;
    return onSearchPacketTail_l(data, len);
}

const char *RtpSplitter::onSearchPacketTail_l(const char *data, uint64_t len) {
    if (_is_jt1078) {
        //找到下一个1078的标记，两个标记间即为一个完整的1078类rtp包
        int next_1078_flag = -1;
        for (int i = 4; i < len - sizeof(kJT1078_MAGIC); i++) {
            if (0 == memcmp(data + i, kJT1078_MAGIC, sizeof(kJT1078_MAGIC) - 1)) {
                next_1078_flag = i;
                break;
            }
        }
        if (next_1078_flag > 0) {
            return data + next_1078_flag;
        }
        return nullptr;
    }
    else {
        //这是rtp包
        uint16_t length = (((uint8_t*)data)[0] << 8) | ((uint8_t*)data)[1];
        if (len < length + 2) {
            //数据不够
            return nullptr;
        }
        //返回rtp包末尾
        return data + 2 + length;
    }
}

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)