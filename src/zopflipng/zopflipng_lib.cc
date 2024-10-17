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
Author: cryopng@free.fr (Frederic Kayser)

See zopflipng_lib.h
*/

#include "zopflipng_lib.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <vector>

#include "lodepng/lodepng.h"
#include "lodepng/lodepng_util.h"
#include "../zopfli/inthandler.h"
#include "../zopfli/deflate.h"
#include "../zopfli/util.h"

ZopfliPNGOptions::ZopfliPNGOptions()
  : lossy_transparent(0)
  , lossy_8bit(false)
  , auto_filter_strategy(true)
  , keep_colortype(false)
  , use_zopfli(true)
  , num_iterations(15)
  , num_iterations_large(5)
  , block_split_strategy(1)
  , blocksplittingmax(15)
  , lengthscoremax(1024)
  , verbose(2)
  , maxfailiterations(0)
  , findminimumrec(9)
  , ranstatewz(65538)
  , ranstatemod(3)
  , pass(0)
  , mode(0)
  , numthreads(1)
  , numthreadsstop(0)
  , statimportance(100)
  , threadaffinity(NULL)
  , affamount(0)
  , smallestblock(1024)
  , testrecmui(20)
  , slowdynmui(0)
  , try_paletteless_size(2048)
  , ga_population_size(19)
  , ga_max_evaluations(0)
  , ga_stagnate_evaluations(15)
  , ga_mutation_probability(0.01)
  , ga_crossover_probability(0.9)
  , ga_tournament_size(2)
  , ga_number_of_offspring(2) {
}

// Deflate compressor passed as fuction pointer to LodePNG to have it use Zopfli
// as its compression backend.
unsigned CustomPNGDeflate(unsigned char** out, size_t* outsize,
                          const unsigned char* in, size_t insize,
                          const LodePNGCompressSettings* settings) {
  const ZopfliPNGOptions* png_options =
      static_cast<const ZopfliPNGOptions*>(settings->custom_context);
  unsigned char bp = 0;
  ZopfliOptions options;
  ZopfliInitOptions(&options);

  options.numiterations     = insize < 200000
                            ? png_options->num_iterations
                            : png_options->num_iterations_large;
  options.blocksplittingmax = png_options->blocksplittingmax;
  options.lengthscoremax    = png_options->lengthscoremax;
  options.verbose           = png_options->verbose;
  mui                       = png_options->maxfailiterations;
  options.findminimumrec    = png_options->findminimumrec;
  options.ranstatewz        = png_options->ranstatewz;
  options.ranstatemod       = png_options->ranstatemod;
  options.pass              = png_options->pass;
  options.mode              = png_options->mode;
  options.numthreads        = png_options->numthreads;
  options.statimportance    = png_options->statimportance;
  options.threadaffinity    = png_options->threadaffinity;
  options.affamount         = png_options->affamount;
  options.smallestblock     = png_options->smallestblock;
  options.testrecmui        = png_options->testrecmui;
  options.slowdynmui        = png_options->slowdynmui;

  ZopfliDeflate(&options, 2 /* Dynamic */, 1, in, insize, &bp, out, outsize, 0);

  return 0;  // OK
}

// Counts amount of colors in the image, up to 257. If transparent_counts_as_one
// is enabled, any color with alpha channel 0 is treated as a single color with
// index 0.
void CountColors(std::set<uint32_t>* unique,
                 const uint32_t* image, unsigned w, unsigned h,
                 bool transparent_counts_as_one) {
  unique->clear();
  for (size_t i = 0; i < w * h; ++i) {
    unique->insert(transparent_counts_as_one
                   && ((unsigned char*)&image[i])[3] == 0 ? 0 : image[i]);
    if (unique->size() > 256) break;
  }
}

// Prepare image for PNG-32 to PNG-24(+tRNS) or PNG-8(+tRNS) reduction.
void TryColorReduction(lodepng::State* inputstate, uint32_t* image,
                       unsigned w, unsigned h) {
  // First look for binary (all or nothing) transparency color-key based.
  bool key = true;
  for (size_t i = 0; i < w * h; ++i) {
    const unsigned char trans = ((unsigned char*)&image[i])[3];
    if (trans > 0 && trans < 255) {
      key = false;
      break;
    }
  }
  std::set<uint32_t> count;  // Color count, up to 257.
  CountColors(&count, image, w, h, true);
  // Less than 257 colors means a palette could be used.
  bool palette = count.size() <= 256;

  // Choose the color key or first initial background color.
  if (key || palette) {
    uint32_t rgb = 0;
    for (size_t i = 0; i < w * h; ++i) {
      if (((unsigned char*)&image[i])[3] == 0) {
        // Use RGB value of first encountered transparent pixel. This can be
        // used as a valid color key, or in case of palette ensures a color
        // existing in the input image palette is used.
        rgb = image[i];
        break;
      }
    }
    for (size_t i = 0; i < w * h; ++i) {
      // if alpha is 0, set the RGB value to the sole color-key.
      if (((unsigned char*)&image[i])[3] == 0) image[i] = rgb;
    }
      // If there are now less colors, update palette of input image to match
      // this.
    if (palette && inputstate->info_png.color.palettesize > 0) {
      CountColors(&count, image, w, h, false);
      if (count.size() < inputstate->info_png.color.palettesize) {
        std::vector<uint32_t> palette_out;
        uint32_t* palette_in = (uint32_t*)(inputstate->info_png.color.palette);
        for (size_t i = 0; i < inputstate->info_png.color.palettesize; i++) {
          if (count.count(palette_in[i]) != 0) {
            palette_out.push_back(palette_in[i]);
          }
        }
        inputstate->info_png.color.palettesize = palette_out.size();
        std::copy(palette_in, palette_in + palette_out.size(),
                  palette_out.begin());
      }
    }
  }
}

// Remove RGB information from pixels with alpha=0 (does the same job as
// cryopng)
void LossyOptimizeTransparent(unsigned char* image, unsigned w, unsigned h,
                              int cleaner) {
  int pre = 0, pgr = 0, pbl = 0;
  uint32_t* image_int = (uint32_t*)image;
  switch (cleaner) {
    case 1: // None filter
      for (size_t i = 0; i < w * h; ++i) {
        // if alpha is 0, set the RGB values to zero (black).
        if (image[(i << 2) + 3] == 0) image_int[i] = 0;
      }
      break;
    case 2: // Sub filter
      for (size_t i = 0; i < ((w << 2) * h); i += (w << 2)) {
        for (size_t j = 3; j < (w << 2); j += 4) {
          // if alpha is 0, set the RGB values to those of the pixel on the
          // left.
          if (image[i + j] == 0) {
            image[i + j - 3] = pre;
            image[i + j - 2] = pgr;
            image[i + j - 1] = pbl;
          } else {
            // Use the last encountered RGB value.
            pre = image[i + j - 3];
            pgr = image[i + j - 2];
            pbl = image[i + j - 1];
          }
        }
        if (w > 1) {
          for (size_t j = ((w - 2) << 2) + 3; j + 1 > 0; j -= 4) {
            // if alpha is 0, set the RGB values to those of the pixel on the
            // right.
            if (image[i + j] == 0) {
              image[i + j - 3] = pre;
              image[i + j - 2] = pgr;
              image[i + j - 1] = pbl;
            } else {
              // Use the last encountered RGB value.
              pre = image[i + j - 3];
              pgr = image[i + j - 2];
              pbl = image[i + j - 1];
            }
          }
        }
        pre = pgr = pbl = 0;   // reset to zero at each new line
      }
      break;
    case 3: // Up filter
      for (size_t j = 3; j < (w << 2); j += 4) {
        // if alpha is 0, set the RGB values to zero (black), first line only.
        if (image[j] == 0) {
          image[j - 3] = 0;
          image[j - 2] = 0;
          image[j - 1] = 0;
        }
      }
      if (h > 1) {
        for (size_t j = 3; j < (w << 2); j += 4) {
          for (size_t i = (w << 2); i < ((w << 2) * h); i += (w << 2)) {
            // if alpha is 0, set the RGB values to those of the upper pixel.
            if (image[i + j] == 0) {
              image[i + j - 3] = image[i + j - 3 - (w << 2)];
              image[i + j - 2] = image[i + j - 2 - (w << 2)];
              image[i + j - 1] = image[i + j - 1 - (w << 2)];
            }
          }
          for (size_t i = (w << 2) * (h - 2); i + (w << 2) > 0; i -= (w << 2)) {
            // if alpha is 0, set the RGB values to those of the lower pixel.
            if (image[i + j] == 0) {
              image[i + j - 3] = image[i + j - 3 + (w << 2)];
              image[i + j - 2] = image[i + j - 2 + (w << 2)];
              image[i + j - 1] = image[i + j - 1 + (w << 2)];
            }
          }
        }
      }
      break;
    case 4: // Average filter
      for (size_t j = 3; j < (w << 2); j += 4) {
        // if alpha is 0, set the RGB values to the half of those of the pixel
        // on the left, first line only.
        if (image[j] == 0) {
          pre = pre >> 1;
          pgr = pgr >> 1;
          pbl = pbl >> 1;
          image[j - 3] = pre;
          image[j - 2] = pgr;
          image[j - 1] = pbl;
        } else {
          pre = image[j - 3];
          pgr = image[j - 2];
          pbl = image[j - 1];
        }
      }
      if (h > 1) {
        for (size_t i = (w << 2); i < ((w << 2) * h); i += (w << 2)) {
          pre = pgr = pbl = 0;   // reset to zero at each new line
          for (size_t j = 3; j < (w << 2); j += 4) {
            // if alpha is 0, set the RGB values to the half of the sum of the
            // pixel on the left and the upper pixel.
            if (image[i + j] == 0) {
              pre = (pre + (int)image[i + j - (3 + (w << 2))]) >> 1;
              pgr = (pgr + (int)image[i + j - (2 + (w << 2))]) >> 1;
              pbl = (pbl + (int)image[i + j - (1 + (w << 2))]) >> 1;
              image[i + j - 3] = pre;
              image[i + j - 2] = pgr;
              image[i + j - 1] = pbl;
            } else {
              pre = image[i + j - 3];
              pgr = image[i + j - 2];
              pbl = image[i + j - 1];
            }
          }
        }
      }
      break;
    case 5: // Paeth filter
      for (size_t j = 3; j < (w << 2); j += 4) {  // First line (border effects)
        // if alpha is 0, alter the RGB value to a possibly more efficient one.
        if (image[j] == 0) {
          image[j - 3] = pre;
          image[j - 2] = pgr;
          image[j - 1] = pbl;
        } else {
          pre = image[j - 3];
          pgr = image[j - 2];
          pbl = image[j - 1];
        }
      }
      if (h > 1) {
        int a, b, c, pa, pb, pc, p;
        for (size_t i = (w << 2); i < ((w << 2) * h); i += (w << 2)) {
          pre = pgr = pbl = 0;   // reset to zero at each new line
          for (size_t j = 3; j < (w << 2); j += 4) {
            // if alpha is 0, set the RGB values to the Paeth predictor.
            if (image[i + j] == 0) {
              if (j != 3) {  // not in first column
                a = pre;
                b = (int)image[i + j - (3 + (w << 2))];
                c = (int)image[i + j - (7 + (w << 2))];
                p = b - c;
                pc = a - c;
                pa = abs(p);
                pb = abs(pc);
                pc = abs(p + pc);
                pre = (pa <= pb && pa <=pc) ? a : (pb <= pc) ? b : c;

                a = pgr;
                b = (int)image[i + j - (2 + (w << 2))];
                c = (int)image[i + j - (6 + (w << 2))];
                p = b - c;
                pc = a - c;
                pa = abs(p);
                pb = abs(pc);
                pc = abs(p + pc);
                pgr = (pa <= pb && pa <=pc) ? a : (pb <= pc) ? b : c;

                a = pbl;
                b = (int)image[i + j - (1 + (w << 2))];
                c = (int)image[i + j - (5 + (w << 2))];
                p = b - c;
                pc = a - c;
                pa = abs(p);
                pb = abs(pc);
                pc = abs(p + pc);
                pbl = (pa <= pb && pa <=pc) ? a : (pb <= pc) ? b : c;

                image[i + j - 3] = pre;
                image[i + j - 2] = pgr;
                image[i + j - 1] = pbl;
              } else {
                // first column, set the RGB values to those of the upper pixel.
                pre = (int)image[i + j - (3 + (w << 2))];
                pgr = (int)image[i + j - (2 + (w << 2))];
                pbl = (int)image[i + j - (1 + (w << 2))];
                image[i + j - 3] = pre;
                image[i + j - 2] = pgr;
                image[i + j - 1] = pbl;
              }
            } else {
              pre = image[i + j - 3];
              pgr = image[i + j - 2];
              pbl = image[i + j - 1];
            }
          }
        }
      }
      break;
    case 6: // None filter (white)
      for (size_t i = 0; i < w * h; i += 4) {
        if (image[i + 3] == 0) {
          // if alpha is 0, set the RGB values to 255 (white).
          image[i] = 255;
          image[i + 1] = 255;
          image[i + 2] = 255;
        }
      }
      break;
  }
}

// Tries to optimize given a single PNG filter strategy.
// Returns 0 if ok, other value for error
unsigned TryOptimize(
    const std::vector<unsigned char>& image, unsigned w, unsigned h,
    const lodepng::State& inputstate, bool bit16, bool keep_colortype,
    bool try_paletteless, const std::vector<unsigned char>& origfile,
    ZopfliPNGFilterStrategy filterstrategy,
    ZopfliPNGPalettePriority palette_priority,
    ZopfliPNGPaletteDirection palette_direction,
    ZopfliPNGPaletteTransparency palette_transparency,
    ZopfliPNGPaletteOrder palette_order,
    bool use_zopfli, int windowsize, const ZopfliPNGOptions* png_options,
    std::vector<unsigned char>* out, unsigned char* filterbank,
    lodepng::State& outputstate) {
  unsigned error = 0;
  lodepng::State state;
  std::vector<unsigned char> out2;
  state.encoder.verbose = png_options->verbose;
  state.encoder.zlibsettings.windowsize = windowsize;
  state.encoder.zlibsettings.nicematch = 258;

  if (use_zopfli && png_options->use_zopfli) {
    state.encoder.zlibsettings.custom_deflate = CustomPNGDeflate;
    state.encoder.zlibsettings.custom_context = png_options;
  }

  if (keep_colortype) {
    state.encoder.auto_convert = 0;
    lodepng_color_mode_copy(&state.info_png.color, &inputstate.info_png.color);
  }
  if (inputstate.info_png.color.colortype == LCT_PALETTE) {
    // Make it preserve the original palette order
    lodepng_color_mode_copy(&state.info_raw, &inputstate.info_png.color);
    state.info_raw.colortype = LCT_RGBA;
    state.info_raw.bitdepth = 8;
  }
  if (bit16) {
    state.info_raw.bitdepth = 16;
  }

  state.encoder.filter_palette_zero = 0;

  std::vector<unsigned char> filters;
  switch (filterstrategy) {
    case kStrategyZero:
      state.encoder.filter_strategy = LFS_ZERO;
      break;
    case kStrategyMinSum:
      state.encoder.filter_strategy = LFS_MINSUM;
      break;
    case kStrategyDistinctBytes:
      state.encoder.filter_strategy = LFS_DISTINCT_BYTES;
      break;
    case kStrategyDistinctBigrams:
      state.encoder.filter_strategy = LFS_DISTINCT_BIGRAMS;
      break;
    case kStrategyEntropy:
      state.encoder.filter_strategy = LFS_ENTROPY;
      break;
    case kStrategyBruteForce:
      state.encoder.filter_strategy = LFS_BRUTE_FORCE;
      break;
    case kStrategyIncremental:
      state.encoder.filter_strategy = LFS_INCREMENTAL;
      break;
    case kStrategyGeneticAlgorithm:
      state.encoder.filter_strategy = LFS_GENETIC_ALGORITHM;
      state.encoder.predefined_filters = filterbank;
      state.encoder.ga.number_of_generations = png_options->ga_max_evaluations;
      state.encoder.ga.number_of_stagnations =
        png_options->ga_stagnate_evaluations;
      state.encoder.ga.population_size = png_options->ga_population_size;
      state.encoder.ga.mutation_probability =
        png_options->ga_mutation_probability;
      state.encoder.ga.crossover_probability =
        png_options->ga_crossover_probability;
      state.encoder.ga.tournament_size =
        png_options->ga_tournament_size;
      state.encoder.ga.number_of_offspring =
        std::min(png_options->ga_number_of_offspring,
                 png_options->ga_population_size);
      break;
    case kStrategyOne:
      state.encoder.filter_strategy = LFS_ONE;
      break;
    case kStrategyTwo:
      state.encoder.filter_strategy = LFS_TWO;
      break;
    case kStrategyThree:
      state.encoder.filter_strategy = LFS_THREE;
      break;
    case kStrategyFour:
      state.encoder.filter_strategy = LFS_FOUR;
      break;
    case kStrategyPredefined:
      lodepng::getFilterTypes(filters, origfile);
      if (filters.size() != h) return 1;  // Error getting filters
      state.encoder.filter_strategy = LFS_PREDEFINED;
      state.encoder.predefined_filters = &filters[0];
      break;
    default:
      break;
  }
  switch(palette_priority) {
    case kPriorityPopularity:
      state.encoder.palette_priority = LPPS_POPULARITY;
      break;
    case kPriorityRGB:
      state.encoder.palette_priority = LPPS_RGB;
      break;
    case kPriorityYUV:
      state.encoder.palette_priority = LPPS_YUV;
      break;
    case kPriorityLab:
      state.encoder.palette_priority = LPPS_LAB;
      break;
    case kPriorityMSB:
      state.encoder.palette_priority = LPPS_MSB;
      break;
    default:
      break;
  }
  switch(palette_direction) {
    case kDirectionAscending:
      state.encoder.palette_direction = LPDS_ASCENDING;
      break;
    case kDirectionDescending:
      state.encoder.palette_direction = LPDS_DESCENDING;
      break;
    default:
      break;
  }
  switch(palette_transparency) {
    case kTransparencyIgnore:
      state.encoder.palette_transparency = LPTS_IGNORE;
      break;
    case kTransparencySort:
      state.encoder.palette_transparency = LPTS_SORT;
      break;
    case kTransparencyFirst:
      state.encoder.palette_transparency = LPTS_FIRST;
      break;
    default:
      break;
  }
  switch(palette_order) {
    case kOrderNone:
      state.encoder.palette_order = LPOS_NONE;
      break;
    case kOrderGlobal:
      state.encoder.palette_order = LPOS_GLOBAL;
      break;
    case kOrderNearest:
      state.encoder.palette_order = LPOS_NEAREST;
      break;
    case kOrderWeight:
      state.encoder.palette_order = LPOS_NEAREST_WEIGHT;
      break;
    case kOrderNeighbor:
      state.encoder.palette_order = LPOS_NEAREST_NEIGHBOR;
    default:
      break;
  }

  state.encoder.add_id = false;
  state.encoder.text_compression = 1;

  error = lodepng::encode(*out, image, w, h, state);
  if (!error) {
    std::vector<unsigned char> temp;
    error = lodepng::decode(temp, w, h, outputstate, *out);
  }

  // For low bit depths, try higher depths, which might result in smaller files.
  for (unsigned i = outputstate.info_png.color.bitdepth << 1; i <= 8; i <<= 1) {
    if (error) break;
    state.encoder.auto_convert = 0;
    lodepng_color_mode_copy(&state.info_png.color, &outputstate.info_png.color);
    state.info_png.color.bitdepth = i;
    out2.clear();
    error = lodepng::encode(out2, image, w, h, state);
    if (!error && out2.size() < out->size()) out->swap(out2);
  }

  // For very small output, also try without palette, it may be smaller thanks
  // to no palette storage overhead.
  if (!error && out->size() < (unsigned) png_options->try_paletteless_size
      && !keep_colortype && try_paletteless
      && outputstate.info_png.color.colortype == LCT_PALETTE) {
    if (png_options->verbose) {
      printf("Palette was used,"
             " compressed result is small enough to also try RGB or gray.\n");
    }
    out2.clear();
    LodePNGColorStats stats;
    lodepng_color_stats_init(&stats);
    lodepng_compute_color_stats(&stats, &image[0], w, h, &state.info_raw);
    // Too small for tRNS chunk overhead.
    if (w * h <= 16 && stats.key) stats.alpha = 1;
    state.encoder.auto_convert = 0;
    state.info_png.color.colortype = (stats.alpha ? LCT_RGBA : LCT_RGB);
    state.info_png.color.bitdepth = 8;
    state.info_png.color.key_defined = (stats.key && !stats.alpha);
    if (state.info_png.color.key_defined) {
      state.info_png.color.key_defined = 1;
      state.info_png.color.key_r = (stats.key_r & 255u);
      state.info_png.color.key_g = (stats.key_g & 255u);
      state.info_png.color.key_b = (stats.key_b & 255u);
    }

    error = lodepng::encode(out2, image, w, h, state);
    if (!error && out2.size() < out->size()) {
      out->swap(out2);
    }
  }

  if (error) {
    printf("Encoding error %u: %s\n", error, lodepng_error_text(error));
    return error;
  }

  return 0;
}

// Outputs the intersection of keepnames and non-essential chunks which are in
// the PNG image.
void ChunksToKeep(const std::vector<unsigned char>& origpng,
                  const std::vector<std::string>& keepnames,
                  std::set<std::string>* result) {
  std::vector<std::string> names[3];
  std::vector<std::vector<unsigned char> > chunks[3];

  lodepng::getChunks(names, chunks, origpng);

  for (size_t i = 0; i < 3; i++) {
    for (size_t j = 0; j < names[i].size(); j++) {
      for (size_t k = 0; k < keepnames.size(); k++) {
        if (keepnames[k] == names[i][j]) {
          result->insert(names[i][j]);
        }
      }
    }
  }
}

// Keeps chunks with given names from the original png by literally copying them
// into the new png
void KeepChunks(const std::vector<unsigned char>& origpng,
                const std::vector<std::string>& keepnames,
                std::vector<unsigned char>* png) {
  std::vector<std::string> names[3];
  std::vector<std::vector<unsigned char> > chunks[3];

  lodepng::getChunks(names, chunks, origpng);
  std::vector<std::vector<unsigned char> > keepchunks[3];

  // There are 3 distinct locations in a PNG file for chunks: between IHDR and
  // PLTE, between PLTE and IDAT, and between IDAT and IEND. Keep each chunk at
  // its corresponding location in the new PNG.
  for (size_t i = 0; i < 3; i++) {
    for (size_t j = 0; j < names[i].size(); j++) {
      for (size_t k = 0; k < keepnames.size(); k++) {
        if (keepnames[k] == names[i][j]) {
          keepchunks[i].push_back(chunks[i][j]);
        }
      }
    }
  }

  lodepng::insertChunks(*png, keepchunks);
}

int ZopfliPNGOptimize(const std::vector<unsigned char>& origpng,
    const ZopfliPNGOptions& png_options,
    int verbose,
    std::vector<unsigned char>* resultpng) {
  // Use the largest possible deflate window size
  int windowsize = 32768;

  ZopfliPNGFilterStrategy filterstrategies[kNumFilterStrategies] = {
    kStrategyZero, kStrategyOne, kStrategyTwo, kStrategyThree, kStrategyFour,
    kStrategyMinSum, kStrategyDistinctBytes, kStrategyDistinctBigrams,
    kStrategyEntropy, kStrategyBruteForce, kStrategyIncremental,
    kStrategyPredefined, kStrategyGeneticAlgorithm
  };
  std::string strategy_name[kNumFilterStrategies] = {
    "zero", "one", "two", "three", "four", "minimum_sum", "distinct_bytes",
    "distinct_bigrams", "entropy", "brute_force", "incremental_brute_force",
    "predefined", "genetic_algorithm"
  };
  ZopfliPNGPalettePriority palette_priorities[kNumPalettePriorities] = {
    kPriorityPopularity, kPriorityRGB, kPriorityYUV, kPriorityLab, kPriorityMSB
  };
  std::string priority_name[kNumPalettePriorities] = {
    "popularity", "rgb", "yuv", "lab", "msb"
  };
  ZopfliPNGPaletteDirection palette_directions[kNumPaletteDirections] = {
    kDirectionAscending, kDirectionDescending
  };
  std::string direction_name[kNumPaletteDirections] = {
    "ascending", "descending"
  };
  ZopfliPNGPaletteTransparency palette_transparencies[kNumPaletteTransparencies]
      = {
    kTransparencyIgnore, kTransparencySort, kTransparencyFirst
  };
  std::string transparency_name[kNumPaletteTransparencies] = {
    "ignore", "sort", "first"
  };
  ZopfliPNGPaletteOrder palette_orders[kNumPaletteOrders] = {
    kOrderNone, kOrderGlobal, kOrderNearest, kOrderWeight, kOrderNeighbor
  };
  std::string order_name[kNumPaletteOrders] = {
    "predefined", "global", "nearest", "nearest_weighted", "nearest_neighbor"
  };
  std::string cleaner_name[7] = {
    "none", "black", "horizontal", "vertical", "average", "paeth", "white"
  };
  const int pre_predefined = 10;
  unsigned strategy_enable = 0;
  if (png_options.filter_strategies.empty()) {
    strategy_enable = (1 << kNumFilterStrategies) - 1;
  }
  else {
    for (size_t i = 0; i < png_options.filter_strategies.size(); i++) {
      strategy_enable |=
        (1 << png_options.filter_strategies[filterstrategies[i]]);
    }
  }
  unsigned palette_priority_enable = 0;
  if (png_options.palette_priorities.empty()) {
    palette_priority_enable = (1 << kNumPalettePriorities) - 1;
  }
  else {
    for (size_t i = 0; i < png_options.palette_priorities.size(); i++) {
      palette_priority_enable |=
          (1 << png_options.palette_priorities[palette_priorities[i]]);
    }
  }
  unsigned palette_direction_enable = 0;
  if (png_options.palette_directions.empty()) {
    palette_direction_enable = (1 << kNumPaletteDirections) - 1;
  }
  else {
    for (size_t i = 0; i < png_options.palette_directions.size(); i++) {
      palette_direction_enable |=
          (1 << png_options.palette_directions[palette_directions[i]]);
    }
  }
  unsigned palette_transparency_enable = 0;
  if (png_options.palette_transparencies.empty()) {
    palette_transparency_enable = (1 << kNumPaletteTransparencies) - 1;
  }
  else {
    for (size_t i = 0; i < png_options.palette_transparencies.size(); i++) {
      palette_transparency_enable |=
          (1 << png_options.palette_transparencies[palette_transparencies[i]]);
    }
  }
  unsigned palette_order_enable = 0;
  if (png_options.palette_orders.empty()) {
    palette_order_enable = (1 << kNumPaletteOrders) - 1;
  }
  else {
    for (size_t i = 0; i < png_options.palette_orders.size(); i++) {
      palette_order_enable |=
          (1 << png_options.palette_orders[palette_orders[i]]);
    }
  }
  std::vector<unsigned char> image;
  unsigned w, h;
  unsigned error;
  lodepng::State inputstate;
  error = lodepng::decode(image, w, h, inputstate, origpng);

  bool keep_colortype = png_options.keep_colortype;

  if (!png_options.keepchunks.empty()) {
    // If the user wants to keep the non-essential chunks bKGD or sBIT, the
    // input color type has to be kept since the chunks format depend on it.
    // This may severely hurt compression if it is not an ideal color type.
    // Ideally these chunks should not be kept for web images. Handling of bKGD
    // chunks could be improved by changing its color type but not done yet due
    // to its additional complexity, for sBIT such improvement is usually not
    // possible.
    std::set<std::string> keepchunks;
    ChunksToKeep(origpng, png_options.keepchunks, &keepchunks);
    if (keepchunks.count("bKGD") || keepchunks.count("sBIT")) {
      if (!keep_colortype && verbose) {
        printf("Forced to keep original color type due to keeping bKGD or sBIT"
               " chunk.\n");
      }
      keep_colortype = true;
    }
  }

  if (error) {
    if (verbose) {
      if (error == 1) {
        printf("Decoding error\n");
      } else {
        printf("Decoding error %u: %s\n", error, lodepng_error_text(error));
      }
    }
    return error;
  }

  bool bit16 = false;  // Using 16-bit per channel raw image
  if (inputstate.info_png.color.bitdepth == 16 &&
      (keep_colortype || !png_options.lossy_8bit)) {
    // Decode as 16-bit
    image.clear();
    error = lodepng::decode(image, w, h, origpng, LCT_RGBA, 16);
    bit16 = true;
  }

  std::vector<std::vector<unsigned char>> images;
  if (!error) {
    std::vector<unsigned char> filter;
    std::vector<unsigned char> temp;
    std::vector<unsigned char> predefined;
    if (strategy_enable & (1 << kStrategyPredefined)) {
      lodepng::getFilterTypes(predefined, origpng);
    }
    size_t bestsize = SIZE_MAX;
    unsigned bestcleaner = 0;
    lodepng::State beststate;

    unsigned numcleaners = 1;
    if (!bit16 && png_options.lossy_transparent > 0) {
      TryColorReduction(&inputstate, (uint32_t*)&image[0], w, h);
    }
    lodepng_color_mode_copy(&beststate.info_png.color,
                            &inputstate.info_png.color);

    bool has_transparent = false;
    for (size_t i = 0; i < w * h; i++) {
      if (image[(i << 2) + 3] == 0) {
        has_transparent = true;
        break;
      }
    }
    if (!has_transparent) {
      palette_transparency_enable = 1 << kTransparencyIgnore;
    } else if (!bit16 && png_options.lossy_transparent > 0) numcleaners = 7;

    std::set<unsigned> count1;
    CountColors(&count1, (uint32_t*)&image[0], w, h, false);

    std::vector<unsigned char> orig_image = image;
    for (unsigned j = 0; j < numcleaners; ++j) {
      unsigned cleaner = 1 << j;
      unsigned realj = j;
      // If lossy_transparent, remove RGB information from pixels with alpha=0
      if (png_options.lossy_transparent > 0 && has_transparent) {
        if (!(png_options.lossy_transparent & cleaner)) continue;
        image = orig_image;
        LossyOptimizeTransparent(&image[0], w, h, j);
        if (count1.size() <= 256) {
          std::set<unsigned> count2;
          CountColors(&count2, (uint32_t*)&image[0], w, h, false);
          if (count2.size() > 256) {
            if (j == numcleaners - 1 && images.size() == 0) {
              images.push_back(orig_image);
              realj = 0;
            } else continue;
          }
        }
        bool duplicate = false;
        for (size_t i = 0; i < images.size(); ++i) {
          if (images[i] == image) {
            duplicate = true;
            break;
          }
        }
        if (duplicate) continue;
        images.push_back(image);
      }

      unsigned numpriorities = kNumPalettePriorities;
      unsigned numdirections = kNumPaletteDirections;
      unsigned numtransparencies = kNumPaletteTransparencies;
      unsigned numorders = kNumPaletteOrders;
      // Check whether image can be paletted
      std::set<unsigned> count;
      CountColors(&count, (uint32_t*)&image[0], w, h, false);
      if (count.size() > 256) {
        numpriorities = 1;
        palette_priority_enable = 1;
        numdirections = 1;
        palette_direction_enable = 1;
        numtransparencies = 1;
        palette_transparency_enable = 1;
        numorders = 1;
        palette_order_enable = 1 << kOrderNone;
      }
      bool none_done = false;
      bool first_filter = true;
      bool try_paletteless = true;
      for (unsigned pp = 0; pp < numpriorities; ++pp) {
        if (!(palette_priority_enable & (1 << pp))) continue;
        for (unsigned pd = 0; pd < numdirections; ++pd) {
          if (!(palette_direction_enable & (1 << pd))) continue;
          for (unsigned pt = 0; pt < numtransparencies; ++pt) {
            if (!(palette_transparency_enable & (1 << pt))) continue;
            for (unsigned po = 0; po < numorders; ++po) {
              if (!(palette_order_enable & (1 << po))) continue;
              if (palette_orders[po] == kOrderNone) {
                if (none_done) continue;
                none_done = true;
              }

              std::vector<unsigned char> filterbank;
              // initialize random filters for genetic algorithm
              if (strategy_enable & (1 << kStrategyGeneticAlgorithm)) {
                filterbank.resize(h * std::max(int(kNumFilterStrategies),
                                               png_options.ga_population_size));
                lodepng::randomFilter(filterbank);
              }
              first_filter = true;
              lodepng::State state;
              for (int i = 0; i < kNumFilterStrategies; ++i) {
                if (!(strategy_enable & (1 << i))) continue;
                temp.clear();
                // If auto_filter_strategy, use fast compression to check which
                // PNG filter strategy gives the smallest output. This allows to
                // then do the slow and good compression only on that filter
                // type.

                error = TryOptimize(image, w, h, inputstate, bit16,
                                    first_filter ? keep_colortype
                                    : state.info_png.color.colortype
                                      == LCT_PALETTE,
                                    try_paletteless, origpng,
                                    filterstrategies[i],
                                    palette_priorities[pp],
                                    palette_directions[pd],
                                    palette_transparencies[pt],
                                    first_filter ? palette_orders[po]
                                                 : kOrderNone,
                                    !png_options.auto_filter_strategy
                                    /* use_zopfli */, windowsize,
                                    &png_options, &temp, &filterbank[0], state);
                if (first_filter) {
                  lodepng_state_copy(&inputstate, &state);
                  first_filter = false;
                }
                if (!error) {
                  if (verbose) {
                    if (png_options.lossy_transparent & cleaner
                        && has_transparent) {
                      printf("Cleaner %s ", cleaner_name[realj].c_str());
                    }
                    if (count.size() <= 256) {
                      printf("Palette ");
                      if (po != kOrderNone) {
                        printf("%s %s ", priority_name[pp].c_str(),
                               direction_name[pd].c_str());
                        if (has_transparent) {
                          printf("%s ", transparency_name[pt].c_str());
                        }
                      }
                      printf("%s ", order_name[po].c_str());
                    }
                    printf("Filter %s: %d bytes\n", strategy_name[i].c_str(), (int) temp.size());
                  }
                  if ((strategy_enable & (1 << kStrategyPredefined)
                      && i <= pre_predefined)
                      || strategy_enable & (1 << kStrategyGeneticAlgorithm)) {
                    lodepng::getFilterTypes(filter, temp);
                  }
                  // Skip predefined if already covered by another strategy
                  if (strategy_enable & (1 << kStrategyPredefined)
                      && i <= pre_predefined && predefined == filter) {
                    strategy_enable &= ~(1 << kStrategyPredefined);
                  }
                  // Store filter for use in genetic algorithm seeding
                  if (strategy_enable & (1 << kStrategyGeneticAlgorithm)) {
                    std::copy(filter.begin(), filter.end(),
                              filterbank.begin() + i * h);
                  }
                  if (temp.size() < bestsize) {
                    bestsize = temp.size();
                    lodepng_state_copy(&beststate, &state);
                    bestcleaner = images.size();
                    // Store best result so far in the output.
                    (*resultpng).swap(temp);
                  }
                }
                try_paletteless = false;
              }
            }
          }
        }
      }
    }
    if (png_options.auto_filter_strategy) {
      temp.clear();
      if (png_options.lossy_transparent > 0 && has_transparent) {
        image.swap(images[bestcleaner - 1]);
      }
      error = TryOptimize(image, w, h, beststate, bit16,
                          true /* keep_colortype */,
                          true /* try_paletteless */, *resultpng,
                          kStrategyPredefined, kPriorityNA, kDirectionNA,
                          kTransparencyNA, kOrderNone, true /* use_zopfli */,
                          windowsize, &png_options, &temp, NULL, beststate);
      if (!error && temp.size() < bestsize) (*resultpng).swap(temp);
    }
  }

  if (!error) {
    if (!png_options.keepchunks.empty()) {
      KeepChunks(origpng, png_options.keepchunks, resultpng);
    }
  }

  return error;
}

extern "C" void CZopfliPNGSetDefaults(CZopfliPNGOptions* png_options) {

  memset(png_options, 0, sizeof(*png_options));
  // Constructor sets the defaults
  ZopfliPNGOptions opts;

  png_options->lossy_transparent        = opts.lossy_transparent;
  png_options->lossy_8bit               = opts.lossy_8bit;
  png_options->auto_filter_strategy     = opts.auto_filter_strategy;
  png_options->use_zopfli               = opts.use_zopfli;
  png_options->num_iterations           = opts.num_iterations;
  png_options->num_iterations_large     = opts.num_iterations_large;
  png_options->block_split_strategy     = opts.block_split_strategy;
  png_options->blocksplittingmax        = opts.blocksplittingmax;
  png_options->lengthscoremax           = opts.lengthscoremax;
  png_options->verbose                  = opts.verbose;
  png_options->maxfailiterations        = opts.maxfailiterations;
  png_options->findminimumrec           = opts.findminimumrec;
  png_options->ranstatewz               = opts.ranstatewz;
  png_options->ranstatemod              = opts.ranstatemod;
  png_options->pass                     = opts.pass;
  png_options->mode                     = opts.mode;
  png_options->numthreads               = opts.numthreads;
  png_options->statimportance           = opts.statimportance;
  png_options->threadaffinity           = opts.threadaffinity;
  png_options->affamount                = opts.affamount;
  png_options->smallestblock            = opts.smallestblock;
  png_options->testrecmui               = opts.testrecmui;
  png_options->slowdynmui               = opts.slowdynmui;
  png_options->try_paletteless_size     = opts.try_paletteless_size;
  png_options->ga_population_size       = opts.ga_population_size;
  png_options->ga_max_evaluations       = opts.ga_max_evaluations;
  png_options->ga_stagnate_evaluations  = opts.ga_stagnate_evaluations;
  png_options->ga_mutation_probability  = opts.ga_mutation_probability;
  png_options->ga_crossover_probability = opts.ga_crossover_probability;
  png_options->ga_tournament_size       = opts.ga_tournament_size;
  png_options->ga_number_of_offspring   = opts.ga_number_of_offspring;
}

extern "C" int CZopfliPNGOptimize(const unsigned char* origpng,
                                  const size_t origpng_size,
                                  const CZopfliPNGOptions* png_options,
                                  int verbose,
                                  unsigned char** resultpng,
                                  size_t* resultpng_size) {
  ZopfliPNGOptions opts;

  // Copy over to the C++-style struct
  opts.lossy_transparent        = png_options->lossy_transparent;
  opts.lossy_8bit               = !!png_options->lossy_8bit;
  opts.auto_filter_strategy     = !!png_options->auto_filter_strategy;
  opts.use_zopfli               = !!png_options->use_zopfli;
  opts.num_iterations           = png_options->num_iterations;
  opts.num_iterations_large     = png_options->num_iterations_large;
  opts.block_split_strategy     = png_options->block_split_strategy;
  opts.blocksplittingmax        = png_options->blocksplittingmax;
  opts.lengthscoremax           = png_options->lengthscoremax;
  opts.verbose                  = png_options->verbose;
  opts.maxfailiterations        = png_options->maxfailiterations;
  opts.findminimumrec           = png_options->findminimumrec;
  opts.ranstatewz               = png_options->ranstatewz;
  opts.ranstatemod              = png_options->ranstatemod;
  opts.pass                     = png_options->pass;
  opts.mode                     = png_options->mode;
  opts.numthreads               = png_options->numthreads;
  opts.statimportance           = png_options->statimportance;
  opts.threadaffinity           = png_options->threadaffinity;
  opts.affamount                = png_options->affamount;
  opts.try_paletteless_size     = png_options->try_paletteless_size;
  opts.ga_population_size       = png_options->ga_population_size;
  opts.ga_max_evaluations       = png_options->ga_max_evaluations;
  opts.ga_stagnate_evaluations  = png_options->ga_stagnate_evaluations;
  opts.ga_mutation_probability  = png_options->ga_mutation_probability;
  opts.ga_crossover_probability = png_options->ga_crossover_probability;
  opts.ga_tournament_size       = png_options->ga_tournament_size;
  opts.ga_number_of_offspring   = png_options->ga_number_of_offspring;

  for (int i = 0; i < png_options->num_filter_strategies; i++) {
    opts.filter_strategies.push_back(png_options->filter_strategies[i]);
  }
  for (int i = 0; i < png_options->num_palette_priorities; i++) {
    opts.palette_priorities.push_back(png_options->palette_priorities[i]);
  }
  for (int i = 0; i < png_options->num_palette_directions; i++) {
    opts.palette_directions.push_back(png_options->palette_directions[i]);
  }
  for (int i = 0; i < png_options->num_palette_transparencies; i++) {
    opts.palette_transparencies.push_back(
        png_options->palette_transparencies[i]);
  }
  for (int i = 0; i < png_options->num_palette_orders; i++) {
    opts.palette_orders.push_back(png_options->palette_orders[i]);
  }

  for (int i = 0; i < png_options->num_keepchunks; i++) {
    opts.keepchunks.push_back(png_options->keepchunks[i]);
  }

  const std::vector<unsigned char> origpng_cc(origpng, origpng + origpng_size);
  std::vector<unsigned char> resultpng_cc;

  int ret = ZopfliPNGOptimize(origpng_cc, opts, verbose, &resultpng_cc);
  if (ret) {
    return ret;
  }

  *resultpng_size = resultpng_cc.size();
  *resultpng      = (unsigned char*) malloc(resultpng_cc.size());
  if (!(*resultpng)) {
    return ENOMEM;
  }

  memcpy(*resultpng,
         reinterpret_cast<unsigned char*>(&resultpng_cc[0]),
         resultpng_cc.size());

  return 0;
}
