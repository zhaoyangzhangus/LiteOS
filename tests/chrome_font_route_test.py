#!/usr/bin/env python3
"""Compile the actual Blink family-selection branch for guest and host."""
from chromium_source import chromium_source
import pathlib
import subprocess
import tempfile

source = (chromium_source() / 'third_party/blink/renderer/platform/fonts/skia/font_cache_skia.cc').read_text()
start = source.index('#if defined(__NATIVE_OS__)', source.index('sk_sp<SkTypeface> FontCache::MatchFamilyStyle('))
branch = source[start:source.index('#endif', start) + len('#endif')]
mock = r'''
#include <cassert>
#include <memory>
#include <initializer_list>
#include <cstring>
template<class T> using sk_sp=std::shared_ptr<T>;
struct SkTypeface {};
struct SkFontStyle { int weight; };
static int bundled,system_calls;
static const char* seen_family;
static int seen_weight;
struct Manager {
 sk_sp<SkTypeface> matchFamilyStyle(const char* f,SkFontStyle s) {
  ++system_calls; seen_family=f; seen_weight=s.weight; return {};
 }
};
namespace skia {
sk_sp<SkTypeface> MakeTypefaceFromName(const char* f,SkFontStyle s) {
 ++bundled; seen_family=f; seen_weight=s.weight;
 return std::make_shared<SkTypeface>();
}
}
static sk_sp<SkTypeface> route(const char* family_name,SkFontStyle style) {
 Manager manager; [[maybe_unused]] Manager* fm=&manager;
'''
cases = r'''
 return typeface;
}
int main() {
 for (const char* name : {"Times New Roman","Arial","serif","sans-serif"}) {
  auto face=route(name,{700});
  assert(seen_family==name && seen_weight==700);
#if defined(__NATIVE_OS__)
  assert(face && system_calls==0);
#else
  assert(!face && bundled==0);
#endif
 }
 auto face=route(nullptr,{400});
 assert(!seen_family && seen_weight==400);
#if defined(__NATIVE_OS__)
 assert(face && bundled==5);
#else
 assert(!face && system_calls==5);
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='system-blink-font-test-') as directory:
    for guest in (False, True):
        binary=str(pathlib.Path(directory)/('guest' if guest else 'host'))
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                        *(['-D__NATIVE_OS__'] if guest else []),
                        '-x','c++','-','-o',binary],
                       input=mock+branch+cases,text=True,check=True)
        subprocess.run([binary],check=True)
print('Blink font route: guest bundled loader and unchanged host lookup passed')
