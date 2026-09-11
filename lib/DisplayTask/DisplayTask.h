#pragma once

/**
 * Display task. Sole owner of the ST7735/SPI bus. Drains the display
 * mailbox, renders the layout for whatever DisplayMode arrives, and
 * enforces a ~33ms frame-rate floor. Rendering is self-contained here,
 * not delegated to any other module -- this task owns its own
 * panel-driving code. See DisplayTask.cpp's top-of-file comment for
 * the redraw strategy.
 */

/** Creates and starts the Display task. */
void createDisplayTask();
