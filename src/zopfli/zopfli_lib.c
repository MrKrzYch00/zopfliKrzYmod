/*
Copyright 2011 Google Inc. All Rights Reserved.
Copyright 2015 Mr_KrzYch00. All Rights Reserved.

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

#include "zopfli.h"

#include "deflate.h"
#include "gzip_container.h"
#include "zip_container.h"
#include "zlib_container.h"
#include "inthandler.h"
#include <stdio.h>

void intHandler(int exit_code);

/* You can use this function in Your own lib calls/applications.
   ZopfliOptions and ZopfliFormat structures are REQUIRED.
   ZopfliAdditionalData structure is optional and can be NULL,
   however in case of ZIP You would need to deploy Your own
   CRC32 calculation and input size detection to be updated
   using correct FilePK offset. Additional time support would
   need to be deployed as well but in this case You would need
   to update it also in Central Directory Structure of ZIP file
   at correect offset.
*/

__declspec( dllexport ) void ZopfliCompress(ZopfliOptions* options, ZopfliFormat output_type,
                    const unsigned char* in, size_t insize,
                    unsigned char** out, size_t* outsize, ZopfliAdditionalData* moredata) {
  ZopfliOptions optionstemp;
  ZopfliOptions* optionslib = NULL;
  if(in == NULL || out == NULL || outsize == NULL) {
    fprintf(stderr,"Critical Error: one or more required pointers are NULL\n");
    exit(EXIT_FAILURE);
  }
  if(options == NULL) {
    optionslib = &optionstemp;
    ZopfliInitOptions(optionslib);
    optionslib->verbose = 0;
  } else {
    optionslib = options;
    mui = options->maxfailiterations;
  }
  if (output_type == ZOPFLI_FORMAT_GZIP) {
    ZopfliGzipCompress(optionslib, in, insize, out, outsize, moredata);
  } else if (output_type == ZOPFLI_FORMAT_ZLIB) {
    ZopfliZlibCompress(optionslib, in, insize, out, outsize, moredata);
  } else if (output_type == ZOPFLI_FORMAT_ZIP) {
    ZipCDIR zipcdir;
    InitCDIR(&zipcdir);
    zipcdir.rootdir=(char*)realloc(zipcdir.rootdir,3*sizeof(char*));
    zipcdir.rootdir[0]='.';
    zipcdir.rootdir[1]='/';
    zipcdir.rootdir[2]='\0';
    ZopfliZipCompress(optionslib, in, insize, out, outsize, &zipcdir, moredata);
    free(zipcdir.rootdir);
    free(zipcdir.data);
    free(zipcdir.enddata);
  } else if (output_type == ZOPFLI_FORMAT_DEFLATE) {
    short final = 1;
    unsigned char bp = 0;
    if(moredata != NULL) {
      if(moredata->fullsize<insize) moredata->fullsize=insize;
      final = !moredata->havemoredata;
      bp = moredata->bit_pointer;
    }
    ZopfliDeflate(optionslib, 2 /* Dynamic block */, final,
                  in, insize, &bp, out, outsize, moredata);
  } else {
    fprintf(stderr,"Error: No output format specified\n");
    exit (EXIT_FAILURE);
  }
}
