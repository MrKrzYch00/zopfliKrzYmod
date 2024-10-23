/*
Copyright 2024 Mr_KrzYch00. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: Mr_KrzYch00

Define some additional information for compiler
required by Zopfli KrzYmod.
*/

#ifndef DEFINES_H_
#define DEFINES_H_

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <float.h>

#define VERYEAR   24
#define VERMONTH  10
#define VERCOMMIT 2

#define BESTSTATSDBVER 1

#if (_XOPEN_SOURCE<500)
 #define _XOPEN_SOURCE 500
#endif

#ifndef _THREAD_SAFE
 #define _THREAD_SAFE
#endif

#ifdef NDOUBLE
 #define __STDC_VERSION__ 199901L
 #define ZOPFLI_INVLOG2 1.4426950f
 #define ZOPFLI_CLOSENEGATIVE -1e-5f
 #define ZLOG(x) logf(x)
 typedef float zfloat;
 typedef float zpfloat;
#else
 typedef double zpfloat;
 #ifdef LDOUBLE
  #define __STDC_VERSION__ 199901L
  #if (LDBL_DIG>=33)
   #define ZOPFLI_INVLOG2 1.44269504088896340735992468100189214L
   #define ZOPFLI_CLOSENEGATIVE -1e-32L
  #else
   #if (LDBL_DIG>15)
    #define ZOPFLI_INVLOG2 1.442695040888963407L
    #define ZOPFLI_CLOSENEGATIVE -1e-17L
   #else
    #define ZOPFLI_INVLOG2 1.442695040888963L
    #define ZOPFLI_CLOSENEGATIVE -1e-14L
   #endif
  #endif
  #define ZLOG(x) logl(x)
  typedef long double zfloat;
 #else
  #define ZOPFLI_INVLOG2 1.442695040888963
  #define ZOPFLI_CLOSENEGATIVE -1e-14
  #define ZLOG(x) log(x)
  typedef double zfloat;
 #endif
#endif

#if defined _WIN32 || defined __CYGWIN__
 #ifdef __GNUC__
  #define DLL_PUBLIC __attribute__ ((dllexport))
 #else
  #define DLL_PUBLIC __declspec(dllexport)
 #endif
#else
 #if __GNUC__ >= 4
  #define DLL_PUBLIC __attribute__ ((visibility ("default")))
 #else
  #define DLL_PUBLIC
 #endif
#endif

#define _FILE_OFFSET_BITS 64

#endif
