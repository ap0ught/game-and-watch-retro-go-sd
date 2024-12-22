#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "main.h"
#include "gw_malloc.h"

static uint32_t current_ram_pointer;
uint32_t ram_start;
extern uint32_t __RAM_EMU_END__;

static uint32_t current_ahb_pointer;
extern uint32_t __ahbram_start__;
extern uint32_t __ahbram_end__;
extern uint16_t __AHBRAM_LENGTH__;

static uint32_t current_itc_pointer;
extern uint32_t __itcram_start__;
extern uint32_t __itcram_end__;
extern uint16_t __ITCMRAM_LENGTH__;
extern uint16_t __NULLPTR_LENGTH__;

/* Ram allocation here is simple and does not support free or reallocation  */
/* What is possible is to reinitialize all allocated buffers by calling the */
/* AHB/ITC init function.                                                   */

/* AHB RAM is 128kB, but it's not cached (as it's used for audio DMA it can't)
   so access to data in AHB RAM is slower than in other RAMs, only use this RAM
   for not time critical ressources */
void ahb_init() {
  current_ram_pointer = (uint32_t)0;
  current_ahb_pointer = (uint32_t)(&__ahbram_end__);
}

void *ram_malloc(size_t size) {
  assert(ram_start != 0); // We are not supposed to ram alloc without initializing ram start pointer
  if (current_ram_pointer == 0)
    current_ram_pointer = (ram_start + 3) & ~0x03; // Make sure pointers are always 32 bits aligned;
//  printf("ram_malloc 0x%lx size %d\n",current_ram_pointer,size);
  void *pointer = (void *)current_ram_pointer;
  if (pointer == 0)
    return NULL;
  if ((current_ram_pointer + size) <= ((uint32_t)&__RAM_EMU_END__)) {
    current_ram_pointer = (current_ram_pointer + size + 3) & ~0x03; // Make sure pointers are always 32 bits aligned
    return pointer;
  } else {
    return NULL;
  }
}

void *ram_calloc(size_t count,size_t size) {
  void *pointer = ram_malloc(count*size);
  if (pointer)
    memset(pointer,0,count*size);
  return pointer;
}

void *ahb_malloc(size_t size) {
  void *pointer;
  if (current_ram_pointer != 0)
  {
    pointer = ram_malloc(size);
    if (pointer)
      return pointer;
  }
  pointer = (void *)current_ahb_pointer;
//  printf("hab_malloc 0x%lx size %d\n",current_ahb_pointer,size);
  current_ahb_pointer = (current_ahb_pointer + size + 3) & ~0x03; // Make sure pointers are always 32 bits aligned
  assert((current_ahb_pointer) <= ((((uint32_t)&__ahbram_start__) + ((uint32_t)(&__AHBRAM_LENGTH__)))));
  return pointer;
}

void *ahb_calloc(size_t count,size_t size) {
  void *pointer;
  pointer = ram_malloc(size);
  if (pointer == NULL)
    pointer = ahb_malloc(count*size);
  memset(pointer,0,count*size);
  return pointer;
}

/* ITC RAM is 64kB, it's fast RAM and can be used for any purpose */

void itc_init() {
  current_itc_pointer = (uint32_t)(&__itcram_end__);
}

void *itc_malloc(size_t size) {
  // ITC ram start at 0x00000000 so we can't use NULL value to tell if allocation is not possible.
  void *pointer = (void *)0xffffffff;
//  void *pointer = (void *)current_itc_pointer;
//  printf("itc_malloc 0x%lx size %d\n",current_itc_pointer,size);
  if (((current_itc_pointer + size + 3) & ~0x03) <= ((((uint32_t)&__itcram_start__) + ((uint32_t)(&__ITCMRAM_LENGTH__)) - ((uint32_t)(&__NULLPTR_LENGTH__))))) {
    pointer = (void *)current_itc_pointer;
    current_itc_pointer = (current_itc_pointer + size + 3) & ~0x03; // Make sure pointers are always 32 bits aligned;
  }
//  assert((current_itc_pointer) <= ((((uint32_t)&__itcram_start__) + ((uint32_t)(&__ITCMRAM_LENGTH__)) - ((uint32_t)(&__NULLPTR_LENGTH__)))));
  return pointer;
}

void *itc_calloc(size_t count,size_t size) {
  void *pointer = itc_malloc(count*size);
  if (pointer != (void *)0xffffffff)
    memset(pointer,0,count*size);
  return pointer;
}
