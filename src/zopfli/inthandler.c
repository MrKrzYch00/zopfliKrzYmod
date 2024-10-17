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
*/

#include "defines.h"
#include <stdlib.h>
#include <stdio.h>
#include "inthandler.h"

unsigned int mui;

void intHandler(int exit_code) {
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
