#ifndef FFMSG_H
#define FFMSG_H

enum class FFMsg : int {
    FLUSH = 0,

    Error                = 100,
    Prepared             = 200,
    Completed            = 300,

    VideoSizeChanged     = 400,
    SarChanged           = 401,
    VideoRenderingStart  = 402,
    AudioRenderingStart  = 403,
    VideoRotationChanged = 404,
    AudioDecodedStart    = 405,
    VideoDecodedStart    = 406,

    OpenInput            = 407,
    FindStreamInfo       = 408,
    ComponentOpen        = 409,

    ReqStart = 20001,
    ReqPause = 20002,
    ReqSeek  = 20003,
};

#endif
