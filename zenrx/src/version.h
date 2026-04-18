#pragma once

#define APP_ID        "zenrx"
#define APP_NAME      "ZenRX"
#define APP_DESC      "ZenRX CPU Miner"
#define APP_DOMAIN    "zenplatform.dev"
#define APP_SITE      "zenplatform.dev"
#define APP_COPYRIGHT "Copyright 2025-2026 ZenOS"

#define APP_VER_MAJOR  1
#define APP_VER_MINOR  4
#define APP_VER_PATCH  1

#define ZENRX_STRINGIFY(x) #x
#define ZENRX_TOSTRING(x) ZENRX_STRINGIFY(x)

#define APP_VERSION   ZENRX_TOSTRING(APP_VER_MAJOR) "." ZENRX_TOSTRING(APP_VER_MINOR) "." ZENRX_TOSTRING(APP_VER_PATCH)
#define APP_VERSION_FULL APP_NAME " " APP_VERSION

#ifdef _MSC_VER
#   if (_MSC_VER >= 1930)
#       define MSVC_VERSION 2022
#   elif (_MSC_VER >= 1920 && _MSC_VER < 1930)
#       define MSVC_VERSION 2019
#   elif (_MSC_VER >= 1910 && _MSC_VER < 1920)
#       define MSVC_VERSION 2017
#   elif _MSC_VER == 1900
#       define MSVC_VERSION 2015
#   else
#       define MSVC_VERSION 0
#   endif
#endif
