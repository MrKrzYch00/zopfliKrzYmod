/*
Copyright 2016 Google Inc. All Rights Reserved.
Copyright 2016 Frédéric Kayser. All Rights Reserved.
Copyright 2016 Aaron Kaluszka. All Rights Reserved.
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

Command line tool to recompress and optimize PNG images, using zopflipng_lib.
*/

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
/* Windows priority setter. */
#if _WIN32
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

#include "lodepng/lodepng.h"
#include "zopflipng_lib.h"
#include "lodepng/lodepng_util.h"
#include "../zopfli/inthandler.h"

void intHandlerpng(int exit_code) {
  if(exit_code==2) {
    if(mui == 1) exit(EXIT_FAILURE);
    fprintf(stderr," (!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!)\n"
                   " (!!)                                                               (!!)\n"
                   " (!!) CTRL+C detected! Setting --mui to 1 to finish processing ASAP (!!)\n"
                   " (!!) Block restore points won't be saved from now on               (!!)\n"
                   " (!!) Won't be applied to current block if --t0 was used            (!!)\n"
                   " (!!) Pressing CTRL+C again will TERMINATE the application          (!!)\n");
    fprintf(stderr," (!!)                                                               (!!)\n"
                   " (!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!)\n");
    mui=1;
  }
}

// Returns directory path (including last slash) in dir, filename without
// extension in file, extension (including the dot) in ext
void GetFileNameParts(const std::string& filename,
    std::string* dir, std::string* file, std::string* ext) {
  size_t npos = (size_t)(-1);
  size_t slashpos = filename.find_last_of("/\\");
  std::string nodir;
  if (slashpos == npos) {
    *dir = "";
    nodir = filename;
  } else {
    *dir = filename.substr(0, slashpos + 1);
    nodir = filename.substr(slashpos + 1);
  }
  size_t dotpos = nodir.find_last_of('.');
  if (dotpos == (size_t)(-1)) {
    *file = nodir;
    *ext = "";
  } else {
    *file = nodir.substr(0, dotpos);
    *ext = nodir.substr(dotpos);
  }
}

// Returns whether the file exists and we have read permissions.
bool FileExists(const std::string& filename) {
  FILE* file = fopen(filename.c_str(), "rb");
  if (file) {
    fclose(file);
    return true;
  }
  return false;
}

// Returns the size of the file, if it exists and we have read permissions.
size_t GetFileSize(const std::string& filename) {
  size_t size;
  FILE* file = fopen(filename.c_str(), "rb");
  if (!file) return 0;
  fseek(file , 0 , SEEK_END);
  size = static_cast<size_t>(ftell(file));
  fclose(file);
  return size;
}

void ShowHelp() {
  printf("Usage: zopflipng [OPTIONS] INFILE.PNG OUTFILE.PNG\n"
         "   or: zopflipng [OPTIONS] --prefix=[*] FILE1.PNG FILE2.PNG ...\n"
         "\n"
         "If the output file exists, it is considered a result from\n"
         "a previous run and not overwritten if its filesize is smaller.\n"
         "\n"
         " -------------------------------------\n"
         "           ZOPFLIPNG OPTIONS\n"
         " -------------------------------------\n\n"
         "     # - number       * - string       () - optional\n\n"
         " ************** GENERAL **************\n"
         "-d                         don't save any files (for benchmarking)\n"
         "-y                         always overwrite files\n"
         "--v=[#]                    verbose level for zopfli (0-6, d:2)\n"
         "--prefix=[*]               adds a prefix to output filenames\n"
         "--always_zopflify          always output the image even if bigger\n\n"
         " ********** COMPRESSION TIME *********\n"
         "-m                         use more iterations (depending on file size)\n"
         "-q                         use quick, but not very good compression\n"
         "--iterations=[#]           number of iterations (d:15)\n"
         "--mui=[#]                maximum unsuccessful iterations after last best (d: 0)\n\n"
         " ******** AUTO BLOCK SPLITTER ********\n"
         "--bsr=[#]                  block splitting recursion (min: 2, d: 9)\n"
         "--mb=[#]                   maximum blocks, 0 = unlimited (d: 15)\n"
         "--mls=[#]                  maximum length score (d: 1024)\n"
         "--sb=[#]                   byte-by-byte search when lz77 size < # (d: 1024)\n"
         "--maxrec                   use recursion of lz77 size / bsr times\n"
         "--nosplitlast              don't use splitting last\n"
         "--slowdyn=[#]              LZ77 Optimal in splitter, # - mui\n"
         "--slowfix                  always use expensive fixed block calculations\n"
         "--testrec(=[#])            test recursion, # - 0 or testrec mui (d: 20)\n"
         " ********** COMPRESSION MODE *********\n"
         "--zopfli_filters           use zopfli instead of zlib for every test\n"
         "--all                      use 16 combinations per block and take smallest size\n"
         "--brotli                   use Brotli Huffman optimization\n"
         "--lazy                     lazy matching in Greedy LZ77\n"
         "--ohh                      optymize huffman header\n"
         "--rc                       reverse counts ordering in bit length calculations\n\n"
         " ********* IMAGE OPTIMIZATION ********\n"
         "--lossy_transparent        remove colors behind alpha channel 0\n"
         "--lossy_8bit               convert 16-bit to 8-bit (per channel image)\n"
         "--alpha_cleaners=[*]       remove colors behind alpha channel 0 [bhvapw].\n"
         "--filters=[*]              filter strategies to try [01234mywebipg] (d: all)\n"
         "--palette_priorities=[*]   palette priorities [prylm] (d: all)\n"
         "--palette_directions=[*]   palette directions [ad] (d: all)\n"
         "--palette_transparencies=[*] palette transparencies [isf] (d: all)\n"
         "--palette_orders=[*]       palette orders [pgdwn] (d: all)\n"
         "--try_paletteless_size=[#] don't use palette if < # (d: 2048)\n\n"
         " *** GENETIC ALGORITHM + =[#] all ****\n"
         "--ga_population_size       number of genomes in pool. Default: 19\n"
         "--ga_max_evaluations       overall maximum number of evaluations (d: 0 - all)\n"
         "--ga_stagnate_evaluations  number of sequential evaluations (d: 15)\n"
         "--ga_mutation_probability  probability of mutation per gene per gen. (d: 0.01)\n"
         "--ga_crossover_probability probability of crossover pergeneration (d: 0.9)\n"
         "--ga_number_of_offspring   number of offspring per generation (d: 2)\n"
         "--ga_tournament_size       selecting an individual from a population (d: 2)\n\n"
         " *********** MISCELLANEOUS ***********\n"
         "--keepchunks=[*,*,*...]    keep metadata chunks with these names\n"
         "--keepcolortype            keep original color type and bit depth\n"
         "--t=[#]                    compress using # threads, 0 = compat. (d:1)\n"
         "--tstop=#                  stop early when <= # threads. (d:0)\n"
         "--aff=[#,#,#...]           compression thr. affinity: mask,mask... (d: not set)\n"
         "--idle                     use idle process priority\n"
         "--pass=[#]                 recompress last split points max # times (d: 0)\n"
         "--statsdb                  use file-based best stats / block database\n"
         "--si=[#]                   stats to laststats weight (d: 100, max: 149)\n"
         "--cmwc                     use Complementary-Multiply-With-Carry rand. gen.\n"
         "--rm=[#]                   random modulo for iteration stats (d: 3)\n"
         "--rw=[#]                   initial random W for iteration stats (1-65535, d: 1)\n"
         "--rz=[#]                   initial random Z for iteration stats (1-65535, d: 2)\n"
         "\n");
}

void PrintSize(const char* label, size_t size) {
  printf("%s: %d (%dK)\n", label, (int) size, (int) size / 1024);
}

void PrintResultSize(const char* label, size_t oldsize, size_t newsize) {
  printf("%s: %d (%dK). Percentage of original: %.3f%%\n",
         label, (int) newsize, (int) newsize / 1024, newsize * 100.0 / oldsize);
}

int main(int argc, char *argv[]) {
  printf("ZopfliPNG, a Portable Network Graphics (PNG) image optimizer.\n"
         "KrzYmod extends ZopfliPNG functionality - version %d.%d.%d\n\n",
          VERYEAR, VERMONTH, VERCOMMIT);

  if (argc < 2) {
    ShowHelp();
    return 0;
  }

  ZopfliPNGOptions png_options;

  // cmd line options
  bool always_zopflify = false;  // overwrite file even if we have bigger result
  bool yes = false;  // do not ask to overwrite files
  bool dryrun = false;  // never save anything

  std::string user_out_filename;  // output filename if no prefix is used
  bool use_prefix = false;
  std::string prefix = "zopfli_";  // prefix for output filenames

  std::vector<std::string> files;

  signal(SIGINT, intHandlerpng);

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg[0] == '-' && arg.size() > 1 && arg[1] != '-') {
      for (size_t pos = 1; pos < arg.size(); pos++) {
        char c = arg[pos];
        if (c == 'y') {
          yes = true;
        } else if (c == 'd') {
          dryrun = true;
        } else if (c == 'm') {
          png_options.num_iterations *= 4;
          png_options.num_iterations_large *= 4;
        } else if (c == 'q') {
          png_options.use_zopfli = false;
        } else if (c == 'h') {
          ShowHelp();
          return 0;
        } else {
          printf("Unknown flag: %c\n", c);
          return 0;
        }
      }
    } else if (arg[0] == '-' && arg.size() > 1 && arg[1] == '-') {
      size_t eq = arg.find('=');
      std::string name = arg.substr(0, eq);
      std::string value = eq >= arg.size() - 1 ? "" : arg.substr(eq + 1);
      int num = atoi(value.c_str());
      if (name == "--always_zopflify") {
        always_zopflify = true;
      } else if (name == "--alpha_cleaners") {
        for (size_t j = 0; j < value.size(); j++) {
          char c = value[j];
          int cleaner = 0;
          switch (c) {
            case 'n': cleaner = 0; break;
            case 'b': cleaner = 1; break;
            case 'h': cleaner = 2; break;
            case 'v': cleaner = 3; break;
            case 'a': cleaner = 4; break;
            case 'p': cleaner = 5; break;
            case 'w': cleaner = 6; break;
            default:
              printf("Unknown alpha cleaning method: %c\n", c);
              return 1;
          }
          png_options.lossy_transparent |= (1 << cleaner);
        }
      } else if (name == "--lossy_transparent") {
        png_options.lossy_transparent |= 4;
      } else if (name == "--lossy_8bit") {
        png_options.lossy_8bit = true;
      } else if (name == "--lazy") {
        png_options.mode |= 0x0001;
      } else if (name == "--ohh") {
        png_options.mode |= 0x0002;
      } else if (name == "--rc") {
        png_options.mode |= 0x0004;
      } else if (name == "--brotli") {
        png_options.mode |= 0x0008;
      } else if (name == "--all") {
        png_options.mode |= 0x0010;
      } else if (name == "--cmwc") {
        png_options.mode |= 0x0020;
      } else if (name == "--nosplitlast") {
        png_options.mode |= 0x0040;
      } else if (name == "--slowfix") {
        png_options.mode |= 0x0080;
      } else if (name == "--statsdb") {
        png_options.mode |= 0x0100;
      } else if (name == "--maxrec") {
        png_options.mode |= 0x0200;
      } else if (name == "--testrec") {
        png_options.mode |= 0x0400;
        if(num > 0) png_options.testrecmui = num;
      } else if (name == "--slowdyn") {
        if(num > 0) png_options.slowdynmui = num;
        else        png_options.slowdynmui = 5;
      } else if (name == "--sb") {
        png_options.smallestblock = num;
      } else if (name == "--iterations") {
        png_options.num_iterations = num;
        png_options.num_iterations_large = num;
      } else if (name == "--mb") {
        if (num < 0) num = 15;
        png_options.blocksplittingmax = num;
      } else if (name == "--mls") {
        if (num < 1) num = 1024;
        png_options.lengthscoremax = num;
      } else if (name == "--pass") {
        if (num < 1) num = 1;
        png_options.pass = num;
      } else if (name == "--bsr") {
        if (num < 2) num = 9;
        png_options.findminimumrec = num;
      } else if (name == "--mui") {
        if (num < 0) num = 0;
        png_options.maxfailiterations = num;
      } else if (name == "--v") {
        if (num < 0) num = 1;
        png_options.verbose = num;
      } else if (name == "--si") {
        if (num < 0) num = 1;
        else if(num>149) num = 149;
        png_options.statimportance = num;
      } else if (name == "--rm") {
        if (num < 1) num = 1;
        png_options.ranstatemod = num;
      } else if (name == "--rw") {
        if (num < 1) num = 1;
        if (num > 65535) num = 65535;
        png_options.ranstatewz = (num << 16) + (png_options.ranstatewz & 0xFFFF);
      } else if (name == "--rz") {
        if (num < 1) num = 1;
        if (num > 65535) num = 65535;
        png_options.ranstatewz = num + (png_options.ranstatewz & 0xFFFF0000);
      } else if (name == "--idle") {
        IdlePriority();
      } else if (name == "--tstop") {
        png_options.numthreadsstop = num;
      } else if (name == "--t") {
        png_options.numthreads = num;
      } else if (name == "--aff") {
        char buff[2] = {0, 0};
        png_options.threadaffinity = (size_t*)malloc(sizeof(size_t));
        png_options.threadaffinity[png_options.affamount] = 0;
        for (size_t j = 0; j < value.size(); j++) {
          switch(value[j]) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
              png_options.threadaffinity[png_options.affamount] *= 10;
              buff[0] = value[j];
              png_options.threadaffinity[png_options.affamount] += atoi(buff);
              break;
            case ',':
              ++png_options.affamount;
              png_options.threadaffinity = (size_t*)realloc(png_options.threadaffinity, (png_options.affamount+1) * sizeof(size_t));
              png_options.threadaffinity[png_options.affamount] = 0;
          }
        }
        ++png_options.affamount;
      } else if (name == "--splitting") {
        // ignored
      } else if (name == "--filters") {
        for (size_t j = 0; j < value.size(); j++) {
          ZopfliPNGFilterStrategy strategy = kStrategyZero;
          char f = value[j];
          switch (f) {
            case '0': strategy = kStrategyZero; break;
            case '1': strategy = kStrategyOne; break;
            case '2': strategy = kStrategyTwo; break;
            case '3': strategy = kStrategyThree; break;
            case '4': strategy = kStrategyFour; break;
            case 'm': strategy = kStrategyMinSum; break;
            case 'y': strategy = kStrategyDistinctBytes; break;
            case 'w': strategy = kStrategyDistinctBigrams; break;
            case 'e': strategy = kStrategyEntropy; break;
            case 'b': strategy = kStrategyBruteForce; break;
            case 'i': strategy = kStrategyIncremental; break;
            case 'p': strategy = kStrategyPredefined; break;
            case 'g': strategy = kStrategyGeneticAlgorithm; break;
            default:
              printf("Unknown filter strategy: %c\n", f);
              return 1;
          }
          png_options.filter_strategies.push_back(strategy);
        }
      } else if (name == "--zopfli_filters") {
        png_options.auto_filter_strategy = false;
      } else if (name == "--palette_priorities") {
        for (size_t j = 0; j < value.size(); j++) {
          ZopfliPNGPalettePriority popularity = kPriorityPopularity;
          char p = value[j];
          switch (p) {
            case 'p': popularity = kPriorityPopularity; break;
            case 'r': popularity = kPriorityRGB; break;
            case 'y': popularity = kPriorityYUV; break;
            case 'l': popularity = kPriorityLab; break;
            case 'm': popularity = kPriorityMSB; break;
            default:
              printf("Unknown palette priority: %c\n", p);
              return 1;
          }
          png_options.palette_priorities.push_back(popularity);
        }
      } else if (name == "--palette_directions") {
        for (size_t j = 0; j < value.size(); j++) {
          ZopfliPNGPaletteDirection direction = kDirectionAscending;
          char d = value[j];
          switch (d) {
            case 'a': direction = kDirectionAscending; break;
            case 'd': direction = kDirectionDescending; break;
            default:
              printf("Unknown palette direction: %c\n", d);
              return 1;
          }
          png_options.palette_directions.push_back(direction);
        }
      } else if (name == "--palette_transparencies") {
        for (size_t j = 0; j < value.size(); j++) {
          ZopfliPNGPaletteTransparency transparency = kTransparencyIgnore;
          char t = value[j];
          switch (t) {
            case 'i': transparency = kTransparencyIgnore; break;
            case 's': transparency = kTransparencySort; break;
            case 'f': transparency = kTransparencyFirst; break;
            default:
              printf("Unknown palette direction: %c\n", t);
              return 1;
          }
          png_options.palette_transparencies.push_back(transparency);
        }
      } else if (name == "--palette_orders") {
        for (size_t j = 0; j < value.size(); j++) {
          ZopfliPNGPaletteOrder order = kOrderNone;
          char o = value[j];
          switch (o) {
            case 'p': order = kOrderNone; break;
            case 'g': order = kOrderGlobal; break;
            case 'd': order = kOrderNearest; break;
            case 'w': order = kOrderWeight; break;
            case 'n': order = kOrderNeighbor; break;
            default:
              printf("Unknown palette order: %c\n", o);
              return 1;
          }
          png_options.palette_orders.push_back(order);
        }
      } else if (name == "--try_paletteless_size") {
        if (num < 0) num = 0;
        png_options.try_paletteless_size = num;
      } else if (name == "--keepchunks") {
        bool correct = true;
        if ((value.size() + 1) % 5 != 0) correct = false;
        for (size_t i = 0; i + 4 <= value.size() && correct; i += 5) {
          png_options.keepchunks.push_back(value.substr(i, 4));
          if (i > 4 && value[i - 1] != ',') correct = false;
        }
        if (!correct) {
          printf("Error: keepchunks format must be like for example:\n"
                 " --keepchunks=gAMA,cHRM,sRGB,iCCP\n");
          return 0;
        }
      } else if (name == "--keepcolortype") {
        png_options.keep_colortype = true;
      } else if (name == "--prefix") {
        use_prefix = true;
        if (!value.empty()) prefix = value;
      } else if (name == "--ga_population_size") {
        if (num < 1) num = 1;
        png_options.ga_population_size = num;
      } else if (name == "--ga_max_evaluations") {
        if (num < 0) num = 0;
        png_options.ga_max_evaluations = num;
      } else if (name == "--ga_stagnate_evaluations") {
        if (num < 1) num = 1;
        png_options.ga_stagnate_evaluations = num;
      } else if (name == "--ga_mutation_probability") {
        double numf = atof(value.c_str());
        if (numf < 0.0f) numf = 0.0f;
        else if (numf > 1.0f) numf = 1.0f;
        png_options.ga_mutation_probability = numf;
      } else if (name == "--ga_crossover_probability") {
        double numf = atof(value.c_str());
        if (numf < 0.0f) numf = 0.0f;
        else if (numf > 1.0f) num = 1.0f;
        png_options.ga_crossover_probability = numf;
      } else if (name == "--ga_number_of_offspring") {
        if (num < 1) num = 1;
        png_options.ga_number_of_offspring = num;
      } else if (name == "--ga_tournament_size") {
        if (num < 1) num = 1;
        png_options.ga_tournament_size = num;
      } else if (name == "--help") {
        ShowHelp();
        return 0;
      } else {
        printf("Unknown flag: %s\n", name.c_str());
        return 0;
      }
    } else {
      files.push_back(argv[i]);
    }
  }

  if (!use_prefix) {
    if (files.size() == 2) {
      // The second filename is the output instead of an input if no prefix is
      // given.
      user_out_filename = files[1];
      files.resize(1);
    } else {
      printf("Please provide one input and output filename\n\n");
      ShowHelp();
      return 0;
    }
  }

  size_t total_in_size = 0;
  // Total output size, taking input size if the input file was smaller
  size_t total_out_size = 0;
  // Total output size that zopfli produced, even if input was smaller, for
  // benchmark information
  size_t total_out_size_zopfli = 0;
  size_t total_errors = 0;
  size_t total_files = 0;
  size_t total_files_smaller = 0;
  size_t total_files_saved = 0;
  size_t total_files_equal = 0;

  for (size_t i = 0; i < files.size(); i++) {
    if (use_prefix && files.size() > 1) {
      std::string dir, file, ext;
      GetFileNameParts(files[i], &dir, &file, &ext);
      // avoid doing filenames which were already output by this so that you
      // don't get zopfli_zopfli_zopfli_... files after multiple runs.
      if (file.find(prefix) == 0) continue;
    }

    total_files++;

    printf("Optimizing %s\n", files[i].c_str());
    std::vector<unsigned char> image;
    unsigned w, h;
    std::vector<unsigned char> origpng;
    unsigned error;
    lodepng::State inputstate;
    std::vector<unsigned char> resultpng;

    error = lodepng::load_file(origpng, files[i]);
    if (!error) {
      error = ZopfliPNGOptimize(origpng, png_options,
                                png_options.verbose, &resultpng);
    }

    if (error) {
      if (error == 1) {
        printf("Decoding error\n");
      } else {
        printf("Decoding error %u: %s\n", error, lodepng_error_text(error));
      }
    }

    // Verify result, check that the result causes no decoding errors
    if (!error) {
      error = lodepng::decode(image, w, h, resultpng);
      if (!error) {
        std::vector<unsigned char> origimage;
        unsigned origw, origh;
        lodepng::decode(origimage, origw, origh, origpng);
        if (origw != w || origh != h || origimage.size() != image.size()) {
          error = 1;
        } else {
          for (size_t i = 0; i < image.size(); i += 4) {
            bool same_alpha = image[i + 3] == origimage[i + 3];
            bool same_rgb =
                (png_options.lossy_transparent && image[i + 3] == 0) ||
                (image[i + 0] == origimage[i + 0] &&
                 image[i + 1] == origimage[i + 1] &&
                 image[i + 2] == origimage[i + 2]);
            if (!same_alpha || !same_rgb) {
              error = 1;
              break;
            }
          }
        }
      }
      if (error) {
        printf("Error: verification of result failed, keeping original."
               " Error: %u.\n", error);
        // Reset the error to 0, instead set output back to the original. The
        // input PNG is valid, zopfli failed on it so treat as if it could not
        // make it smaller.
        error = 0;
        resultpng = origpng;
      }
    }

    if (error) {
      total_errors++;
    } else {
      size_t origsize = origpng.size();
      size_t resultsize = resultpng.size();

      if (!png_options.keepchunks.empty()) {
        std::vector<std::string> names;
        std::vector<size_t> sizes;
        lodepng::getChunkInfo(names, sizes, resultpng);
        for (size_t i = 0; i < names.size(); i++) {
          if (names[i] == "bKGD" || names[i] == "sBIT") {
            printf("Forced to keep original color type due to keeping bKGD or"
                   " sBIT chunk. Try without --keepchunks for better"
                   " compression.\n");
            break;
          }
        }
      }

      PrintSize("Input size", origsize);
      PrintResultSize("Result size", origsize, resultsize);
      if (resultsize < origsize) {
        printf("Result is smaller\n");
      } else if (resultsize == origsize) {
        printf("Result has exact same size\n");
      } else {
        printf(always_zopflify
            ? "Original was smaller\n"
            : "Preserving original PNG since it was smaller\n");
      }

      std::string out_filename = user_out_filename;
      if (use_prefix) {
        std::string dir, file, ext;
        GetFileNameParts(files[i], &dir, &file, &ext);
        out_filename = dir;
        out_filename += prefix;
        out_filename += file;
        out_filename += ext;
      }
      bool different_output_name = out_filename != files[i];

      total_in_size += origsize;
      total_out_size_zopfli += resultpng.size();
      if (resultpng.size() < origsize) total_files_smaller++;
      else if (resultpng.size() == origsize) total_files_equal++;

      if (!always_zopflify && resultpng.size() >= origsize) {
        // Set output file to input since zopfli didn't improve it.
        resultpng = origpng;
      }

      bool already_exists = FileExists(out_filename);
      size_t origoutfilesize = GetFileSize(out_filename);

      // When using a prefix, and the output file already exist, assume it's
      // from a previous run. If that file is smaller, it may represent a
      // previous run with different parameters that gave a smaller PNG image.
      // This also applies when not using prefix but same input as output file.
      // In that case, do not overwrite it. This behaviour can be removed by
      // adding the always_zopflify flag.
      bool keep_earlier_output_file = already_exists &&
          resultpng.size() >= origoutfilesize && !always_zopflify &&
          (use_prefix || !different_output_name);

      if (keep_earlier_output_file) {
        // An output file from a previous run is kept, add that files' size
        // to the output size statistics.
        total_out_size += origoutfilesize;
        if (use_prefix) {
          printf(resultpng.size() == origoutfilesize
              ? "File not written because a previous run was as good.\n"
              : "File not written because a previous run was better.\n");
        }
      } else {
        bool confirmed = true;
        if (!yes && !dryrun && already_exists) {
          printf("File %s exists, overwrite? (y/N) ", out_filename.c_str());
          char answer = 0;
          // Read the first character, the others and enter with getchar.
          while (int input = getchar()) {
            if (input == '\n' || input == EOF) break;
            else if (!answer) answer = input;
          }
          confirmed = answer == 'y' || answer == 'Y';
        }
        if (confirmed) {
          if (!dryrun) {
            if (lodepng::save_file(resultpng, out_filename) != 0) {
              printf("Failed to write to file %s\n", out_filename.c_str());
            } else {
              total_files_saved++;
            }
          }
          total_out_size += resultpng.size();
        } else {
          // An output file from a previous run is kept, add that files' size
          // to the output size statistics.
          total_out_size += origoutfilesize;
        }
      }
    }
    printf("\n");
  }

  if (total_files > 1) {
    printf("Summary for all files:\n");
    printf("Files tried: %d\n", (int) total_files);
    printf("Files smaller: %d\n", (int) total_files_smaller);
    if (total_files_equal) {
      printf("Files equal: %d\n", (int) total_files_equal);
    }
    printf("Files saved: %d\n", (int) total_files_saved);
    if (total_errors) printf("Errors: %d\n", (int) total_errors);
    PrintSize("Total input size", total_in_size);
    PrintResultSize("Total output size", total_in_size, total_out_size);
    PrintResultSize("Benchmark result size",
                    total_in_size, total_out_size_zopfli);
  }

  if (dryrun) printf("No files were written because dry run was specified\n");

  return total_errors;
}
