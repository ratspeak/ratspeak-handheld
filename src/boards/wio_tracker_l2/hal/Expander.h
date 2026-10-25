#pragma once
#include <Arduino.h>

// PCA9555 rail and reset controller. Outputs are shadowed so a bit change is
// one write, and a mutex covers the shadow: GNSS power is switched by the
// service task while the UI task polls the wake button.
namespace wiol2::expander {
// Brings the bus up, stages every rail and runs the LCD/touch reset sequence.
bool begin();
bool ready();
bool setOutput(uint8_t bit, bool high);
// Reads the input port; false when the expander did not answer.
bool readInput(uint8_t bit, bool& high);
}
