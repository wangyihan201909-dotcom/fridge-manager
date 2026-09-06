/*
 * 冰箱管家的云端地址。
 *
 * 复制成 fridge_config.h 再填自己的值（fridge_config.h 已 gitignore）：
 *
 *     cp fridge_config.example.h fridge_config.h
 *
 * 这里填微信云开发的 HTTP 访问服务域名，并把 /device 下的五个端点
 * （sync / op / ack / voice / bind，外加 pair）路由到 device 云函数、
 * 开启「路径透传」。不透传的话它们会共用同一个 event.path，服务端没法区分。
 *
 * 不抽成配置而硬编码是不行的：pair 端点免鉴权（设备此刻还没有密钥），
 * 每调一次就建一条数据库文档 —— 地址随开源代码公开等于把一个
 * 不限次的建文档入口交出去。
 */
#ifndef FRIDGE_CONFIG_H
#define FRIDGE_CONFIG_H

#define FRIDGE_CLOUD_BASE "https://your-env-id.ap-shanghai.app.tcloudbase.com"

#endif
