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

/*
Zopfli compressor program. It can output gzip-, zlib- or deflate-compatible
data. By default it creates a .gz file. This tool can only compress, not
decompress. Decompression can be done by any standard gzip, zlib or deflate
decompressor.
*/

#include "defines.h"
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
/* Windows priority setter & stdout fix. */
#if _WIN32
#include <fcntl.h>
#include <windows.h>
static void IdlePriority() {
 if(SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS)==0) {
  fprintf(stderr,"ERROR! Failed setting priority!\n\n");
 } else {
  fprintf(stderr,"INFO: Idle priority successfully set.\n\n");
 }
}
#else
#include <sys/resource.h>
static void IdlePriority() {
 if(setpriority(PRIO_PROCESS, 0, 19)==-1) {
  fprintf(stderr,"ERROR! Failed setting priority!\n\n");
 } else {
  fprintf(stderr,"INFO: Idle priority successfully set.\n\n");
 }
}
#endif

#include "zopfli_bin.h"
#include "util.h"
#include "inthandler.h"
#include "deflate.h"
#include "adler.h"
#include "crc32.h"
#include "blocksplitter.h"

static const char tempfileext[8] = { '.' , 'z' , 'o' , 'p' , 'f' , 'l' , 'i', 0 };

void intHandler(int exit_code);

static void InitCDIR(ZipCDIR *zipcdir) {
  zipcdir->data = NULL;
  zipcdir->size = 0;
  zipcdir->offset = 0;
  zipcdir->fileid = 0;
  zipcdir->fullsize = 0;
}

static void CleanCDIR(ZipCDIR *zipcdir) {
  free(zipcdir->data);
  zipcdir->data = 0;
}

static void ZopfliInitBinOptions(ZopfliBinOptions* options) {
  options->usescandir = 0;
  options->blocksize = 0;
  options->numblocks = 0;
  options->custblocksplit = NULL;
  options->dumpsplitsfile = NULL;
  options->additionalautosplits = 0;
}

static size_t ceilz(zfloat num) {
    size_t inum = (size_t)num;
    if (num == (zfloat)inum) {
        return inum;
    }
    return inum + 1;
}

static int exists(const char* file) {
  FILE* ofile;
  if((ofile = fopen(file, "r")) != NULL) {
    fclose(ofile);
    return 1;
  }
  return 0;
}

/*
Similar to original Zopfli but with support for larger files (especially
for 64 bit compilations), offset reading, applying ZOPFLI_MASTER_BLOCK_SIZE limiter.
*/
static void LoadFile(const char* filename,
                     unsigned char** out, size_t* outsize, size_t* offset,
                     size_t* filesize, size_t amount, int limiter) {
  FILE* file;

  *out = 0;
  *outsize = 0;
  file = fopen(filename, "rb");
  if (!file) return;

  if(*offset==0) {
    unsigned char testfile;
    fseeko(file,(size_t)(-1), SEEK_SET);
    *filesize = ftello(file);
    if(*filesize > 0 && (fread(&testfile, 1, 1, file))==1) {
      fprintf(stderr,"Error: Files larger than %luMB are not supported"
                      "by this version.\n",(unsigned long)((size_t)(-1)/1024/1024));
      exit(EXIT_FAILURE);
    }
  }
  fseeko(file , 0 , SEEK_END);
  *filesize = ftello(file);
  *outsize = *filesize-*offset;
  if(amount>0) *outsize=amount;
  if(*outsize > ZOPFLI_MASTER_BLOCK_SIZE && ZOPFLI_MASTER_BLOCK_SIZE>0
     && limiter==1) {
    *outsize = ZOPFLI_MASTER_BLOCK_SIZE;
  }
  fseeko(file, *offset, SEEK_SET);
  *out = Zmalloc(*outsize);
  *offset+=*outsize;

  if (*outsize && (*out)) {
    size_t testsize = fread(*out, 1, *outsize, file);
    if (testsize != *outsize) {
      /* It could be a directory */
      free(*out);
      *out = 0;
      *outsize = 0;
    }
  }

  fclose(file);
}

/*
Saves a file from a memory array, overwriting the file if it existed.
With offsets support for chunk-writting.
*/
static void SaveFile(const char* filename,
                     const unsigned char* in, size_t insize, size_t fseekdata) {
  FILE* file;
  if(fseekdata==0) {
    file = fopen(filename, "wb");
  } else {
    file = fopen(filename, "r+b");
  }
  if(file == NULL) {
    fprintf(stderr,"Error: Can't write to output file, terminating.\n");
    exit (EXIT_FAILURE);
  }
  fseeko(file,fseekdata,SEEK_SET);
  fwrite((char*)in, 1, insize, file);
  fclose(file);
}

static char StringsEqual(const char* str1, const char* str2) {
  return strcmp(str1, str2) == 0;
}

/*
Output to stdout.
*/
static void ConsoleOutput(const unsigned char* in, size_t insize) {
  /* Windows workaround for stdout output. */
#if _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  fwrite(in, 1, insize, stdout);
#if _WIN32
  _setmode(_fileno(stdout), _O_TEXT);
#endif
}

/*
Add two strings together. Size does not matter. Result must be freed.
*/
static char* AddStrings(const char* str1, const char* str2) {
  int a,b;
  char* result;
  for(a=0;str1[a]!='\0';a++) {}
  for(b=0;str2[b]!='\0';b++) {}
  result = Zmalloc((a+b) + 1);
  strcpy(result, str1);
  strcat(result, str2);
  return result;
}

/*
Scans directory and its subdirectories for files that will be later
compressed to ZIP. Empty directories and 0 size files won't be compressed.
*/
static int ListDir(const char* filename, char ***filesindir,
                   unsigned int *j, int isroot) {
  DIR *dir;
  struct dirent *ent;
  struct stat attrib;
  char* initdir=AddStrings(filename,"/");
  char* statfile=NULL;
  unsigned int i, k, l;
  dir = opendir(filename);
  if(! dir) {
    free(initdir);
    return 0;
  } else {
    while(1) {
      ent = readdir(dir);
      if(! ent) break;
      if(!StringsEqual(ent->d_name,".") && !StringsEqual(ent->d_name,"..")) {
        statfile=AddStrings(initdir,ent->d_name);
        stat(statfile, &attrib);
        free(statfile);
        statfile = 0;
        if((attrib.st_mode & S_IFDIR)==0) {
          *filesindir = Zrealloc(*filesindir,((unsigned int)*j+1)*(sizeof(char*)));
          if(isroot==1) {
            for(i=0;ent->d_name[i]!='\0';i++) {}
            (*filesindir)[*j] = Zmalloc(i * sizeof(char*) +1);
            strcpy((*filesindir)[*j], ent->d_name);
          } else {
            for(i=0;initdir[i]!='/';i++) {}
            ++i;
            for(k=i;initdir[k]!='\0';k++) {}
            k-=i;
            for(l=0;ent->d_name[l]!='\0';l++) {}
            (*filesindir)[*j] = Zmalloc(k+l * sizeof(char*)+1);
            statfile=AddStrings(initdir+i,ent->d_name);
            strcpy((*filesindir)[*j], statfile);
            free(statfile);
            statfile = 0;
          }
          ++*j;
        } else {
          statfile=AddStrings(initdir,ent->d_name);
          ListDir(statfile, filesindir, j,0);
          free(statfile);
          statfile = 0;
        }
      }
    }
    closedir(dir);
  }
  free(initdir);
  return 1;
}

/*
ZIP and GZIP timestamp generator from file creation time.
*/
static unsigned long Timestamp(const char* file, const ZopfliFormat output_type) {
  struct tm* tt;
  struct stat attrib;
  stat(file, &attrib);
  if(output_type == ZOPFLI_FORMAT_GZIP || output_type == ZOPFLI_FORMAT_GZIP_NAME) {
    tt = gmtime(&(attrib.st_mtime));
    if(tt->tm_year<70) {
      tt->tm_year=70;
      mktime(tt);
    }
    return tt->tm_sec + tt->tm_min*60 + tt->tm_hour*3600 + tt->tm_yday*86400 +
            (tt->tm_year-70)*31536000 + ((tt->tm_year-69)/4)*86400 -
            ((tt->tm_year-1)/100)*86400 + ((tt->tm_year+299)/400)*86400;
  } else if(output_type == ZOPFLI_FORMAT_ZIP) {
    tt = localtime(&(attrib.st_mtime));
    if(tt->tm_year<80) {
      tt->tm_year=80;
      mktime(tt);
    }
    return ((tt->tm_year-80) << 25) + ((tt->tm_mon+1) << 21) + (tt->tm_mday << 16) +
           (tt->tm_hour << 11) + (tt->tm_min << 5) + (tt->tm_sec >> 1);
  } else {
    return 0;
  }
}

/*
Renames a file. Will prompt user if destination file already exists.
*/
static void RenameFile(const char* tempfilename, const char* outfilename) {
  int input;
  if(exists(outfilename)) {
    char answer = 0;
    fprintf(stderr,"File %s already exists, overwrite? (y/N) ",outfilename);
    while((input = getchar())) {
      if (input == '\n' || input == EOF) {
        break;
      } else if(!answer) {
        answer = input;
      }
    }
    if(answer == 'y' || answer == 'Y') {
      if(remove(outfilename)!=0) fprintf(stderr,"Error: %s\n",strerror(errno));
    } else {
      fprintf(stderr,"Info: File was not replaced and left as tempfile: %s\n",tempfilename);
      return;
    }
  }
  if(rename(tempfilename,outfilename)!=0) {
    fprintf(stderr,"Error: %s\n",strerror(errno));
  }
}

/*
This procedure would most likely require optimisations since it contains a lot of
(working!) crap, to omitt certain LIB limitations that, on the other hand, would require
a lot of modifications. We don't want to make LIB go too far away from original, do we?
*/
static int Compress(ZopfliOptions* options, const ZopfliBinOptions* binoptions,
                         const ZopfliFormat output_type,
                         const char* infilename,
                         const char* outfilename, ZipCDIR* zipcdir, size_t initsoffset) {
  ZopfliAdditionalData moredata;
  unsigned char* in = NULL;
  unsigned char* out = NULL;
  unsigned char bp = 0;
  unsigned long checksum = 0L;
  size_t insize;
  size_t outsize = 0;
  size_t fullsize;
  size_t compsize;
  size_t loffset = 0;
  size_t soffset = initsoffset;
  size_t pkoffset = 14;
  size_t *splitpoints = NULL;
  size_t offset = 0;
  size_t i, j = 0;
  int blocksplitting = options->blocksplitting;
  int final = 1;
  ZopfliPredefinedSplits sp;

  LoadFile(infilename, &in, &insize, &loffset, &fullsize, 1, 1);
  free(in);
  if (fullsize == 0 || insize == 0) {
    if(options->verbose>0) fprintf(stderr, "Error: Invalid filename: %s\n", infilename);
    return 0;
  } else {
    for(i = 0;infilename[i]!='\0';i++) {}
    if(output_type == ZOPFLI_FORMAT_ZIP && fullsize>(4294967295u-(i*2+98)+soffset)) {
      if(options->verbose>0) fprintf(stderr,"Error: File %s may exceed ZIP limits of 4G\n", infilename);
      return 0;
    }
  }

  if(output_type == ZOPFLI_FORMAT_ZLIB) ++checksum;

  loffset = 0;

  moredata.timestamp = Timestamp(infilename,output_type);

  if(output_type == ZOPFLI_FORMAT_GZIP || output_type == ZOPFLI_FORMAT_GZIP_NAME) {
    static const unsigned char headerstart[3]  = {  31, 139,   8 };
    static const unsigned char headerend[2]    = {   2,   3 };
    for(j=0;j<sizeof(headerstart);++j) ZOPFLI_APPEND_DATA(headerstart[j], &out, &outsize);
    if(output_type == ZOPFLI_FORMAT_GZIP) {
      ZOPFLI_APPEND_DATA(0, &out, &outsize);
    } else {
      ZOPFLI_APPEND_DATA(8, &out, &outsize);
    }

    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((moredata.timestamp >> (j*8)) % 256, &out, &outsize);

    for(j=0;j<sizeof(headerend);++j) ZOPFLI_APPEND_DATA(headerend[j], &out, &outsize);

    if(output_type == ZOPFLI_FORMAT_GZIP_NAME) {
      for(j=0;infilename[j] != '\0';j++) ZOPFLI_APPEND_DATA(infilename[j], &out, &outsize);
      ZOPFLI_APPEND_DATA(0, &out, &outsize);
    }
  } else if(output_type == ZOPFLI_FORMAT_ZLIB) {
    unsigned cmfflg = 30912;
    unsigned fcheck = 31 - cmfflg % 31;
    cmfflg += fcheck;
    ZOPFLI_APPEND_DATA(cmfflg / 256, &out, &outsize);
    ZOPFLI_APPEND_DATA(cmfflg % 256, &out, &outsize);
  } else if(output_type == ZOPFLI_FORMAT_ZIP) {
    unsigned int l;
    static const unsigned char filePKh[10]     = { 80, 75,  3,  4, 20,  0,  2,  0,  8,  0};
    for(j=0;j<sizeof(filePKh);++j) ZOPFLI_APPEND_DATA(filePKh[j], &out, &outsize);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((moredata.timestamp >> (j*8)) % 256, &out, &outsize);
    for(j=0;j<8;++j) ZOPFLI_APPEND_DATA(0, &out, &outsize);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((fullsize >> (j*8)) % 256, &out, &outsize);
    if(zipcdir == NULL) {
      l=0;
    } else {
      for(l=0;infilename[l] != '/';l++) {}
      ++l;
    }
    for(i=0;infilename[i] != '\0';i++) {}
    for(j=0;j<2;++j) ZOPFLI_APPEND_DATA(((i-l) >> (j*8)) % 256, &out, &outsize);
    for(j=0;j<2;++j) ZOPFLI_APPEND_DATA(0, &out, &outsize);
    for(j=l;j<i;++j) ZOPFLI_APPEND_DATA(infilename[j], &out, &outsize);
  }

  offset=outsize;

  sp.splitpoints = 0;
  sp.npoints = 0;
  sp.moresplitting = binoptions->additionalautosplits;
  LoadFile(infilename, &in, &insize, &loffset, &fullsize, 0, 0);
  if(binoptions->custblocksplit != NULL) {
    i=2;
    while(i<=binoptions->custblocksplit[0]) {
      ZOPFLI_APPEND_DATA(binoptions->custblocksplit[i],&sp.splitpoints,&sp.npoints);
      ++i;
    }
    free(binoptions->custblocksplit);
  } else if(binoptions->numblocks>0) {
    if(binoptions->numblocks>1) {
      size_t l;
      if(binoptions->numblocks>fullsize) {
        i = 1;
      } else {
        i = ceilz((zfloat)fullsize / (zfloat)binoptions->numblocks);
      }
      l=i;
      do {
        ZOPFLI_APPEND_DATA(l,&sp.splitpoints,&sp.npoints);
        l+=i;
      } while(l<fullsize);
    }
  } else if(binoptions->blocksize>0 && binoptions->blocksize<fullsize) {
    size_t l;
    i = binoptions->blocksize;
    l=i;
    do {
      ZOPFLI_APPEND_DATA(l,&sp.splitpoints,&sp.npoints);
      l+=i;
    } while(l<fullsize);
  }
  if(output_type == ZOPFLI_FORMAT_GZIP ||
     output_type == ZOPFLI_FORMAT_GZIP_NAME || output_type == ZOPFLI_FORMAT_ZIP) {
    CRCu(in,insize,&checksum);
  } else if(output_type == ZOPFLI_FORMAT_ZLIB) {
    adler32u(in,insize,&checksum);
  }
  ZopfliDeflate(options, 2, final, in, insize, &bp, &out, &outsize, &sp);
  free(in);
  if (!outfilename) {
    ConsoleOutput(out,outsize-1);
  } else {
    SaveFile(outfilename, out, outsize,soffset);
  }
  if(binoptions->dumpsplitsfile != NULL) {
    FILE* file = NULL;
    char* tempfilename = NULL;
    tempfilename = AddStrings(binoptions->dumpsplitsfile,tempfileext);
    file = fopen(tempfilename, "wb");
    fprintf(file,"0");
    if(sp.npoints>0) {
      for (j = 0; j < sp.npoints; j++) {
        fprintf(file, ",%x", (int)sp.splitpoints[j]);
      }
    }
    fclose(file);
    RenameFile(tempfilename,binoptions->dumpsplitsfile);
    fprintf(stderr,"Hex split points successfully saved to file: %s\n",binoptions->dumpsplitsfile);
    free(tempfilename);
  }
  free(sp.splitpoints);

  compsize = outsize+soffset-offset-initsoffset;

  if(output_type == ZOPFLI_FORMAT_GZIP || output_type == ZOPFLI_FORMAT_GZIP_NAME) {
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((checksum >> (j*8)) % 256, &out, &outsize);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((fullsize >> (j*8)) % 256, &out, &outsize);
    offset+=8;
  } else if(output_type == ZOPFLI_FORMAT_ZLIB) {
    for(j=4;j!=0;--j) ZOPFLI_APPEND_DATA((checksum >> ((j-1)*8)) % 256, &out, &outsize);
    offset+=4;
  } else if(output_type == ZOPFLI_FORMAT_ZIP) {
    size_t l;
    static const unsigned char CDIRPKh[12]     = { 80, 75,  1,  2, 20,  0, 20,  0,  2,  0,  8,  0};
    static const unsigned char CDIRPKs[12]     = {  0,  0,  0,  0,  0,  0,  0,  0, 32,  0,  0,  0};
    static const unsigned char EndCDIRPKh[8]   = { 80, 75,  5,  6,  0,  0,  0,  0 };
    ZipCDIR zipcdirtemp;
    ZipCDIR* zipcdirloc = NULL;
    if(zipcdir == NULL) {
      zipcdirloc = &zipcdirtemp;
      InitCDIR(zipcdirloc);
    } else {
      zipcdirloc = zipcdir;
    }
    for(j=0;j<sizeof(CDIRPKh);++j) ZOPFLI_APPEND_DATA(CDIRPKh[j],&zipcdirloc->data,&zipcdirloc->size);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((moredata.timestamp >> (j*8)) % 256,&zipcdirloc->data,&zipcdirloc->size);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((checksum >> (j*8)) % 256,&zipcdirloc->data,&zipcdirloc->size);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((compsize >> (j*8)) % 256,&zipcdirloc->data,&zipcdirloc->size);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((fullsize >> (j*8)) % 256,&zipcdirloc->data,&zipcdirloc->size);
    if(zipcdir == NULL) {
      l=0;
    } else {
      for(l=0;infilename[l] != '/';l++) {}
      ++l;
    }
    for(i=0;infilename[i] != '\0';i++) {}
    for(j=0;j<2;++j) ZOPFLI_APPEND_DATA(((i-l) >> (j*8)) % 256, &zipcdirloc->data, &zipcdirloc->size);
    for(j=0;j<sizeof(CDIRPKs);++j) ZOPFLI_APPEND_DATA(CDIRPKs[j],&zipcdirloc->data,&zipcdirloc->size);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((zipcdirloc->offset >> (j*8)) % 256,&zipcdirloc->data,&zipcdirloc->size);
    pkoffset = zipcdirloc->offset+14;
    zipcdirloc->offset+=compsize+30+(i-l);
    for(j=l;j<i;j++) ZOPFLI_APPEND_DATA(infilename[j],&zipcdirloc->data,&zipcdirloc->size);
    for(j=0; j<zipcdirloc->size; ++j) ZOPFLI_APPEND_DATA(zipcdirloc->data[j], &out, &outsize);
    ++zipcdirloc->fileid;
    for(j=0;j<sizeof(EndCDIRPKh); ++j) ZOPFLI_APPEND_DATA(EndCDIRPKh[j], &out, &outsize);
    for(j=0;j<2;++j) ZOPFLI_APPEND_DATA((zipcdirloc->fileid >> (j*8)) % 256, &out, &outsize);
    for(j=0;j<2;++j) ZOPFLI_APPEND_DATA((zipcdirloc->fileid >> (j*8)) % 256, &out, &outsize);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((zipcdirloc->size >> (j*8)) % 256, &out, &outsize);
    for(j=0;j<4;++j) ZOPFLI_APPEND_DATA((zipcdirloc->offset >> (j*8)) % 256, &out, &outsize);
    for(j=0;j<2;++j) ZOPFLI_APPEND_DATA(0, &out, &outsize);
    offset+=zipcdirloc->size+22;
    if(zipcdir == NULL) {
      CleanCDIR(zipcdirloc);
    } else {
      zipcdirloc->fullsize += fullsize;
      fullsize = zipcdirloc->fullsize;
      zipcdir = zipcdirloc;
    }
  }
  if (!outfilename) {
    ConsoleOutput(out,outsize);
  } else {
    SaveFile(outfilename, out, outsize,soffset);
    if(output_type == ZOPFLI_FORMAT_ZIP) {
      unsigned char* buff = Zmalloc(8 * sizeof(unsigned char*));
      for(i=0;i<4;++i) {
        buff[i] = (checksum >> (i*8)) % 256;
        buff[i+4] = (compsize >> (i*8)) % 256;
      }
      SaveFile(outfilename, buff, 8,pkoffset);
      free(buff);
    }
  }
  free(out);
  free(splitpoints);

  outsize+=soffset;
  compsize = outsize-offset;

  if (options->verbose>1) {
    fprintf(stderr,"---------------------------------\n");
    PrintSummary(fullsize,outsize,compsize);
    fprintf(stderr,"---------------------------------\n");
  }

  options->blocksplitting = blocksplitting;

  return 1;

}

/*
Wrapper for Compress for single-file mode.
*/
static void CompressFile(ZopfliOptions* options, const ZopfliBinOptions* binoptions,
                         const ZopfliFormat output_type,
                         const char* infilename,
                         const char* outfilename) {

  char* tempfilename = NULL;

  mui = options->maxfailiterations;

  if(outfilename) tempfilename = AddStrings(outfilename,tempfileext);

  if(Compress(options,binoptions,output_type,infilename,tempfilename,0,0)==1)
    RenameFile(tempfilename,outfilename);

  free(tempfilename);

}

/*
Wrapper for Compress for multi-file mode.
*/
static void CompressMultiFile(ZopfliOptions* options, const ZopfliBinOptions* binoptions,
                         const ZopfliFormat output_type,
                         const char* infilename,
                         const char* outfilename) {

  ZipCDIR zipcdir;
  char** filesindir = NULL;
  char* tempfilename = NULL;
  char* dirname = NULL;
  char* fileindir = NULL;
  unsigned int j = 0;
  unsigned int i;

  if(ListDir(infilename, &filesindir, &j, 1)==0) {
    fprintf(stderr, "Error: %s is not a directory or doesn't exist.\n",infilename); 
    return;
  } else if(j==0) {
    fprintf(stderr, "Directory %s seems empty.\n", infilename);
    return;
  }

  mui = options->maxfailiterations;

  if(outfilename) tempfilename = AddStrings(outfilename,tempfileext);

  InitCDIR(&zipcdir);

  dirname=AddStrings(infilename, "/");

  for(i = 0; i < j; ++i) {

    fileindir=AddStrings(dirname,filesindir[i]);

    if(options->verbose>1) fprintf(stderr, "--------------------------------------------\n");
    if(options->verbose>0) fprintf(stderr, "[%d / %d] Adding file: %s\n", (i + 1), j, filesindir[i]);
    if(Compress(options,binoptions,output_type,fileindir,tempfilename,&zipcdir,zipcdir.offset)==0) {
      fprintf(stderr,"Error: couldn't add %s to archive -- next.\n",filesindir[i]);
    }

    free(fileindir);
    free(filesindir[i]);
    fileindir = 0;

  }

  CleanCDIR(&zipcdir);

  RenameFile(tempfilename,outfilename);

  free(tempfilename);
  free(filesindir);
    
}

/*
Here we parse custom block split points passed with
CBS[FILE] switch. Intentionally used size_t here.
Note that too high split points on 32-bit architecture
may turn into garbage...
*/
static void ParseCustomBlockBoundaries(size_t** bs, const char* data) {
  char buff[2] = {0, 0};
  size_t j, k=1;
  (*bs) = Zmalloc(++k * sizeof(size_t*));
  (*bs)[0] = 1;
  (*bs)[1] = 0;
  for(j=0;data[j]!='\0';j++) {
    if(data[j]==',') {
      ++(*bs)[0];
      (*bs) = Zrealloc((*bs), ++k * sizeof(size_t*));
      (*bs)[k-1] = 0;
    } else {
      buff[0]=data[j];
      (*bs)[k-1] = ((*bs)[k-1]<<4) + strtoul(buff,NULL,16);
    }
  }
}

static void VersionInfo(void) {
  fprintf(stderr,
  "Zopfli, a Compression Algorithm to produce Deflate streams.\n"
  "KrzYmod extends Zopfli functionality - version %d.%d.%d\n\n",
  VERYEAR, VERMONTH, VERCOMMIT);
}

int main(int argc, char* argv[]) {
  ZopfliOptions options;
  ZopfliBinOptions binoptions;
  ZopfliFormat output_type = ZOPFLI_FORMAT_GZIP;
  const char* filename = 0;
  int output_to_stdout = 0;
  int i;

  signal(SIGINT, intHandler);

  ZopfliInitOptions(&options);
  ZopfliInitBinOptions(&binoptions);

  for (i = 1; i < argc; i++) {
    const char* arg = argv[i];
    if (StringsEqual(arg, "--c")) output_to_stdout = 1;
    else if (StringsEqual(arg, "--deflate")) output_type = ZOPFLI_FORMAT_DEFLATE;
    else if (StringsEqual(arg, "--zlib")) output_type = ZOPFLI_FORMAT_ZLIB;
    else if (StringsEqual(arg, "--gzip")) output_type = ZOPFLI_FORMAT_GZIP;
    else if (StringsEqual(arg, "--gzipname")) output_type = ZOPFLI_FORMAT_GZIP_NAME;
    else if (StringsEqual(arg, "--zip")) output_type = ZOPFLI_FORMAT_ZIP;
    else if (StringsEqual(arg, "--idle")) IdlePriority();
    else if (StringsEqual(arg, "--lazy")) options.mode |= 0x0001;
    else if (StringsEqual(arg, "--ohh")) options.mode |= 0x0002;
    else if (StringsEqual(arg, "--rc")) options.mode |= 0x0004;
    else if (StringsEqual(arg, "--brotli")) options.mode |= 0x0008;
    else if (StringsEqual(arg, "--all")) options.mode |= 0x0010;
    else if (StringsEqual(arg, "--cmwc")) options.mode |= 0x0020;
    else if (StringsEqual(arg, "--nosplitlast")) options.mode |= 0x0040;
    else if (StringsEqual(arg, "--slowfix")) options.mode |= 0x0080;
    else if (StringsEqual(arg, "--statsdb")) options.mode |= 0x0100;
    else if (StringsEqual(arg, "--maxrec")) options.mode |= 0x0200;
    else if (StringsEqual(arg, "--dir")) binoptions.usescandir = 1;
    else if (StringsEqual(arg, "--aas")) binoptions.additionalautosplits = 1;
    else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 's' && arg[3] == 'l'
          && arg[4] == 'o' && arg[5] == 'w' && arg[6] == 'd' && arg[7] == 'y'
          && arg[8] == 'n') {
      if(arg[9] >= '0' && arg[9] <= '9')
        options.slowdynmui = atoi(arg + 9);
      else
        options.slowdynmui = 5;
    } else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 't' && arg[3] == 'e'
            && arg[4] == 's' && arg[5] == 't' && arg[6] == 'r' && arg[7] == 'e'
            && arg[8] == 'c') {
      options.mode |= 0x0400;
      if(arg[9] >= '0' && arg[9] <= '9')
        options.testrecmui = atoi(arg + 9);
    } else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 's' && arg[3] == 'b'
            && arg[4] >= '0' && arg[4] <= '9') {
      options.smallestblock = atoi(arg + 4);
    } else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 's' && arg[3] == 'i'
            && arg[4] >= '0' && arg[4] <= '9') {
      options.statimportance = atoi(arg + 4);
      if (options.statimportance > 149) options.statimportance = 149;
      else if (options.statimportance < 1) options.statimportance = 1;
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'i'
             && arg[3] >= '0' && arg[3] <= '9') {
      options.numiterations = atoi(arg + 3);
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 't') {
      if(arg[3] >= '0' && arg[3] <= '9') {
        options.numthreads = atoi(arg + 3);
      } else if(arg[3] == 's' && arg[4] == 't' && arg[5] == 'o' && arg[6] == 'p'
             && arg[7] >= '0' && arg[7] <= '9') {
        options.numthreadsstop = atoi(arg + 7);
      }
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'a' && arg[3] == 'f'
             && arg[4] == 'f' && arg[5] >= '0' && arg[5] <= '9') {
      const char *aff = arg+5;
      char buff[2] = {0, 0};
      size_t pos = 0;
      options.threadaffinity = Zmalloc(sizeof(size_t));
      while(aff[pos] != '\0') {
        options.threadaffinity[options.affamount] = 0;
        while(aff[pos] != ',') {
          options.threadaffinity[options.affamount] *= 10;
          buff[0] = aff[pos];
          options.threadaffinity[options.affamount] += atoi(buff);
          ++pos;
          if(aff[pos] == '\0') break;
        }
        ++options.affamount;
        options.threadaffinity = Zrealloc(options.threadaffinity, (options.affamount+1) * sizeof(size_t));
        if(aff[pos] == '\0') break;
        ++pos;
      }
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'm' && arg[3] == 'b'
             && arg[4] >= '0' && arg[4] <= '9') {
      options.blocksplittingmax = atoi(arg + 4);
      if (options.blocksplittingmax < 0) options.blocksplittingmax = 0;
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'p' && arg[3] == 'a'
             && arg[4] == 's' && arg[5] == 's' && arg[6] >= '0' && arg[6] <= '9') {
      options.pass = atoi(arg + 6);
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'm' && arg[3] == 'l'
             && arg[4] == 's' && arg[5] >= '0' && arg[5] <= '9') {
      options.lengthscoremax = atoi(arg + 5);
      if (options.lengthscoremax < 1) options.lengthscoremax = 1;
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'b' && arg[3] == 's'
             && arg[4] == 'r' && arg[5] >= '0' && arg[5] <= '9') {
      options.findminimumrec = atoi(arg + 5);
      if (options.findminimumrec < 2) options.findminimumrec = 2;
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'r' && arg[3] == 'w'
             && arg[4] >= '0' && arg[4] <= '9') {
      unsigned short num = atoi(arg + 4);
      if(num < 1) num = 1;
      options.ranstatewz = (num << 16) + (options.ranstatewz & 0xFFFF);
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'r' && arg[3] == 'z'
             && arg[4] >= '0' && arg[4] <= '9') {
      unsigned short num = atoi(arg + 4);
      if(num < 1) num = 1;
      options.ranstatewz = num + (options.ranstatewz & 0xFFFF0000);
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'r' && arg[3] == 'm'
             && arg[4] >= '0' && arg[4] <= '9') {
      options.ranstatemod = atoi(arg + 4);
      if (options.ranstatemod < 1) options.ranstatemod = 1;
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'm' && arg[3] == 'u'
             && arg[4] == 'i' && arg[5] >= '0' && arg[5] <= '9') {
      options.maxfailiterations = atoi(arg + 5);
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'c' && arg[3] == 'b'
             && arg[4] == 's' && arg[5] != '\0') {
      if(arg[5] == 'f' && arg[6] == 'i' && arg[7] == 'l' && arg[8] == 'e'
      && arg[9] != '\0') {
        const char *cbsfile = arg+9;
        FILE* file = fopen(cbsfile, "rb");
        char* filedata = NULL;
        size_t size;
        if(file==NULL) {
          fprintf(stderr,"Error: CBS file %s doesn't exist.\n",cbsfile);
          return EXIT_FAILURE;
        }
        fseeko(file,0,SEEK_END);
        size=ftello(file);
        if(size>0) {
          filedata = Zmalloc((size+1) * sizeof(char*));
          rewind(file);
          if(fread(filedata,1,size,file)) {}
          filedata[size]='\0';
          ParseCustomBlockBoundaries(&binoptions.custblocksplit,filedata);
          free(filedata);
        } else {
          fprintf(stderr,"Error: CBS file %s seems empty.\n",cbsfile);
          return EXIT_FAILURE;
        }
        fclose(file);
      } else {
        ParseCustomBlockBoundaries(&binoptions.custblocksplit,arg+5);
      }
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'c' && arg[3] == 'b'
             && arg[4] == 'd' && arg[5] != '\0') {
       binoptions.dumpsplitsfile = arg+5;
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'v' && arg[3] >= '0'
             && arg[3] <= '9') {
      options.verbose = atoi(arg + 3);
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'b' && arg[3] >= '0'
             && arg[3] <= '9') {
      binoptions.blocksize = atoi(arg + 3);
    }  else if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'n' && arg[3] >= '0'
             && arg[3] <= '9') {
      binoptions.numblocks = atoi(arg + 3);
    }
    else if (arg[0] == '-' && (arg[1] == 'h' || arg[1] == '?' || (arg[1] == '-'
         && (arg[2] == 'h' || arg[2] == '?')))) {
      VersionInfo();
      fprintf(stderr,
          "Usage: zopfli [OPTIONS] FILE\n\n"
          " -------------------------------------\n"
          "             ZOPFLI OPTIONS\n"
          " -------------------------------------\n\n"
          "     # - number       * - string\n\n"
          " ************** GENERAL **************\n"
          "  --dir         accept directory as input, requires: --zip\n"
          "  --h           shows this help (--?, -h, -?)\n"
          "  --v#          verbose level (0-6, d: 2)\n\n");
      fprintf(stderr,
          " ********** COMPRESSION TIME *********\n"
          "  --i#          perform # iterations (d: 15; 0 => 4.2 billion)\n"
          "  --mui#        maximum unsucessful iterations after last best (d: 0)\n\n");
      fprintf(stderr,
          " ******** AUTO BLOCK SPLITTER ********\n"
          "  --bsr#        block splitting recursion (min: 2, d: 9)\n"
          "  --mb#         maximum blocks, 0 = unlimited (d: 15)\n"
          "  --mls#        maximum length score (d: 1024)\n"
          "  --sb#         byte-by-byte search when lz77 size < # (d: 1024)\n"
          "  --maxrec      use recursion of lz77 size / bsr times\n"
          "  --nosplitlast don't use splitting last\n"
          "  --slowdyn#    LZ77 Optimal in splitter, # - mui\n"
          "  --slowfix     always use expensive fixed block calculations\n");
      fprintf(stderr,
          "  --testrec#    test recursion, # - 0 or testrec mui (d: 20)\n\n");
      fprintf(stderr,
          " ******* MANUAL BLOCK SPLITTER *******\n"
          "  --b#          block size in bytes\n"
          "  --n#          number of blocks\n"
          "  --cbs*,*,*... customize block start points\n"
          "                format: hex_startb1,hex_startb2\n"
          "                example: 0,33f0,56dd,8799,22220\n"
          "  --cbsfile*    same as above but instead read from file #\n"
          "  --cbd*        dump block start points to # file and exit\n"
          "  --aas         additional automatic splitting between manual points\n\n");
      fprintf(stderr,
          " ********** COMPRESSION MODE *********\n"
          "  --all         use 16 combinations of 4 switches below\n"
          "  --brotli      use Brotli Huffman optimization\n"
          "  --lazy        lazy matching in Greedy LZ77\n"
          "  --ohh         optimize huffman header\n"
          "  --rc          reverse counts ordering in bit length calculations\n\n");
      fprintf(stderr,
          " *********** OUTPUT CONTROL **********\n"
          "  --c           output to stdout\n"
          "  --deflate     output to deflate format\n"
          "  --gzip        output to gzip format (default)\n"
          "  --gzipname    output to gzip format with filename\n"
          "  --zip         output to zip format\n"
          "  --zlib        output to zlib format\n\n");
      fprintf(stderr,
          " *********** MISCELLANEOUS ***********\n"
          "  --t#          compress using # threads, 0 = compat. (d:1)\n"
          "  --tstop#      stop early when <= # threads. (d:0)\n"
          "  --aff#,#,#... thread affinity: mask,mask,mask... (d: not set)\n"
          "  --idle        use idle process priority\n"
          "  --pass#       recompress last split points max # times (d: 0)\n");
      fprintf(stderr,
          "  --statsdb     use file-based best stats / block database\n"
          "  --si#         stats to laststats in weight calculations (d: 100, max: 149)\n"
          "  --cmwc        use Complementary-Multiply-With-Carry rand. gen.\n"
          "  --rm#         random modulo for iteration stats (d: 3)\n"
          "  --rw#         initial random W for iteration stats (1-65535, d: 1)\n"
          "  --rz#         initial random Z for iteration stats (1-65535, d: 2)\n\n"
          " Pressing CTRL+C will set maximum unsuccessful iterations to 1.\n"
          "\n");
      fprintf(stderr,"Floating point arithmetic precision: %d-bit\n"
                     "Maximum supported input file size: %luMB.\n\n",(int)(8 * sizeof(zfloat)),(unsigned long)((size_t)(-1)/1024/1024));
      return EXIT_FAILURE;
    }
  }
  
  if(options.numthreads > 0 && options.numthreadsstop >= options.numthreads) {
    options.numthreadsstop = options.numthreads - 1;
  }

  if(options.verbose) VersionInfo();

  if((options.mode & 0x0100) && options.verbose) {
    fprintf(stderr, "Info: Using Best Stats database (ZopfliDB directory)\n");
  }

  for (i = 1; i < argc; i++) {
    if (argv[i][0] != '-') {
      char* outfilename;
      filename = argv[i];
      if (output_to_stdout) {
        outfilename = 0;
      } else if (output_type == ZOPFLI_FORMAT_GZIP || output_type == ZOPFLI_FORMAT_GZIP_NAME) {
        outfilename = AddStrings(filename, ".gz");
      } else if (output_type == ZOPFLI_FORMAT_ZLIB) {
        outfilename = AddStrings(filename, ".zlib");
      } else if (output_type == ZOPFLI_FORMAT_ZIP) {
        outfilename = AddStrings(filename, ".zip");
      } else {
        output_type = ZOPFLI_FORMAT_DEFLATE;
        outfilename = AddStrings(filename, ".deflate");
      }
      if (options.verbose && outfilename) {
        fprintf(stderr, "Saving to: %s\n\n", outfilename);
      }
      if(binoptions.usescandir) {
        if(output_type == ZOPFLI_FORMAT_ZIP && !output_to_stdout) {
            if(binoptions.custblocksplit != NULL || binoptions.dumpsplitsfile != NULL) {
              fprintf(stderr, "Error: --cbs and --cbd work only in single file compression (no --dir).\n");
              return EXIT_FAILURE;
            }
          CompressMultiFile(&options, &binoptions, output_type, filename, outfilename);
        } else {
          if(!output_to_stdout) {
            fprintf(stderr, "Error: --dir will only work with ZIP container (--zip).\n");
          } else {
            fprintf(stderr, "Error: Can't output to stdout when compressing multiple files (--dir and --c).\n");
          }
          return EXIT_FAILURE;
        }
      } else {
        CompressFile(&options, &binoptions, output_type, filename, outfilename);
      }
      free(outfilename);
    }
  }

  if (!filename) {
    fprintf(stderr,
            "Error: Please provide filename to compress.\nFor help, type: %s --h\n", argv[0]);
     return EXIT_FAILURE;
  }
  if(options.affamount>0) free(options.threadaffinity);

  return EXIT_SUCCESS;
}
