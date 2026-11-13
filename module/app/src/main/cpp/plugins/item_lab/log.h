#pragma once
#include <cstdarg>
#include <cstdio>
#include "plugin.h"
namespace qol::features::item_lab::private_log {
inline void write(int level, const char* format, ...) {
    const auto* sdk=host_sdk();
    if(!sdk||!sdk->log.write||!format)return;
    char message[contracts::kMessageCap+1]{};
    va_list args;va_start(args,format);const int n=std::vsnprintf(message,sizeof(message),format,args);va_end(args);
    if(n<0)return;
    const char* file=__FILE__;for(const char* p=file;*p;++p)if(*p=='/'||*p=='\\')file=p+1;
    char source[128]{};std::snprintf(source,sizeof(source),"%s",file);
    const uint32_t size=static_cast<uint32_t>(n)<contracts::kMessageCap?static_cast<uint32_t>(n):contracts::kMessageCap;
    (void)sdk->log.write(level,18,source,message,size);
}
}
#define ITEM_LAB_INFO(...) ::qol::features::item_lab::private_log::write(1, __VA_ARGS__)
#define ITEM_LAB_WARN(...) ::qol::features::item_lab::private_log::write(2, __VA_ARGS__)
