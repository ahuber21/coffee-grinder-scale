#pragma once

// Task #4 -- Display. Per rtos-architecture.md §3.2/§6.4: sole owner of the
// ST7735/SPI bus. Drains the display mailbox, renders the layout for
// whatever DisplayMode arrives, and enforces the ~33ms frame-rate floor.
//
// Rendering is self-contained here (not delegated to the old lib/Display/
// module -- that module is left in place for reference only, per AR-026;
// this task owns its own panel driving code, matching every other task's
// "own your implementation" shape). See DisplayTask.cpp's top-of-file
// comment for the redraw strategy.

void createDisplayTask();
