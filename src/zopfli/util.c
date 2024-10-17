/*
Copyright 2011 Google Inc. All Rights Reserved.
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

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

#include <stdio.h>
#include <unistd.h>
#include "defines.h"
#include "util.h"
#include "zopfli.h"

void ZopfliInitOptions(ZopfliOptions* options) {
  options->verbose = 2;
  options->numiterations = 15;
  options->blocksplitting = 1;
  options->blocksplittinglast = 0;
  options->blocksplittingmax = 15;
  options->lengthscoremax = 1024;
  options->maxfailiterations = 0;
  options->findminimumrec = 9;
  options->ranstatewz = 65538;
  options->ranstatemod = 3;
  options->pass = 0;
  options->mode = 0;
  options->numthreads = 1;
  options->numthreadsstop = 0;
  options->statimportance = 100;
  options->threadaffinity = NULL;
  options->affamount = 0;
  options->smallestblock = 1024;
  options->testrecmui = 20;
  options->slowdynmui = 0;
}

void *Zmalloc(size_t size) {
  void *ptr;
#if ZOPFLI_MALLOC_RETRY > 0
  size_t x = ZOPFLI_MALLOC_RETRY;
#endif
  if(size == 0) return NULL;
#if ZOPFLI_MALLOC_RETRY > 0
  for(;x>0;--x) {
#else
  for(;;) {
#endif
    ptr = malloc(size);
    if(ptr == NULL) {
      unsigned long x = (unsigned long)size;
      fprintf(stderr," ERROR: Couldn't allocate %lu bytes of memory!\n",x);
      sleep(60);
    } else {
      break;
    }
  }
  return ptr;
}

void *Zcalloc(size_t ElemAm, size_t ElemSize) {
  void *ptr;
#if ZOPFLI_MALLOC_RETRY > 0
  size_t x = ZOPFLI_MALLOC_RETRY;
#endif
  if(ElemSize == 0 || ElemAm == 0) return NULL;
#if ZOPFLI_MALLOC_RETRY > 0
  for(;x>0;--x) {
#else
  for(;;) {
#endif
    ptr = calloc(ElemAm, ElemSize);
    if(ptr == NULL) {
      unsigned long x = (unsigned long)(ElemAm * ElemSize);
      fprintf(stderr," ERROR: Couldn't allocate %lu bytes of memory!\n",x);
      sleep(60);
    } else {
      break;
    }
  }
  return ptr;
}

void *Zrealloc(void* ptr, size_t size) {
  void *newPtr;
#if ZOPFLI_MALLOC_RETRY > 0
  size_t x = ZOPFLI_MALLOC_RETRY;
#endif
  if(size == 0) return NULL;
#if ZOPFLI_MALLOC_RETRY > 0
  for(;x>0;--x) {
#else
  for(;;) {
#endif
    newPtr = realloc(ptr, size);
    if(newPtr == NULL) {
      unsigned long x = (unsigned long)size;
      fprintf(stderr," ERROR: Couldn't allocate %lu bytes of memory!\n",x);
      sleep(60);
    } else {
      break;
    }
  }
  return newPtr;
}
