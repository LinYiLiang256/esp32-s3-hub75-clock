/*
 * state.h：跨模块共享的运行时状态
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 当前表盘 ID（配网/管理页面修改后立即生效） */
extern volatile int g_face_id;

/* 是否处于 AP 配网模式（true 表示尚未联网，保存配置后需要重启） */
extern volatile bool g_provisioning;

/* 是否处于滚动字幕模式（网页发送文字后进入，选择表盘后退出） */
extern volatile bool g_text_mode;

/* 网页画板的自定义图片（64x64，RGB444 打包，每像素 2 字节） */
extern uint16_t g_custom_image[64 * 64];

#ifdef __cplusplus
}
#endif
