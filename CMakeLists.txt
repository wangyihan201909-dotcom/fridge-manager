# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 版本号刻意抬到 999 段：**上游的发布永远追不上，开机时的 OTA 自动升级
# 就不会把这台设备刷成没有冰箱管家的构建。**
#
# 起因：application.cc 在每次开机会拿这个版本去问 CONFIG_OTA_URL
# （api.tenclass.net，**他们的服务器，不是我们的**），对方返回的版本更高就
# 静默下载刷写并重启，不问用户。而实测他们确实为本板返回了版本与下载地址，
# 只是当时不高于我们继承来的 2.4.2 —— 上游一发 2.4.3 就会中招。
#
# **这一手只挡版本漂移，挡不住两件事**：服务器回 force:1 会跳过版本比对
# （ota.cc）；self.upgrade_firmware 这个 MCP 工具收任意 URL 就刷，它的
# user_only 只是列表过滤加一个 annotation，tools/call 时设备不做拦截。
# 要挡那两条，得改成只接受我们自己主机的固件 URL。
#
# 999 后面两段是我们自己的版本，正常迭代照常往上加。
#
# OTA 端点不能直接关：它同时返回 MQTT 端点、激活信息和服务器时间
# （修时钟那次改的就是它）。
set(PROJECT_VER "999.1.0")
project(xiaozhi)
