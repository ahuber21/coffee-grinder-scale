#pragma once

// Task #4 -- Display. Per rtos-architecture.md §3.2/§6.4: sole owner of the
// ST7735/SPI bus. This skeleton stubs the actual panel init and rendering
// (real ST7735 drawing is follow-up work, per the task brief) -- what's real
// here is the mailbox drain, change-detection against the last rendered
// DisplayCommand, and the ~33ms frame-rate floor.

void createDisplayTask();
