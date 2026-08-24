# Plan for Creating a Separate Headunit Project with ESP32 for GC9A01 Display

## Objective  
Establish a separate ESP32 project for a headunit that integrates a GC9A01 display and displays a historical analog clock.

## Tasks Overview

1. **Bestandsanalyse**:
   - Inspect existing projects' structure.
   - Identify common components, potential conflicts, and available assets.

2. **Recherche**:
   - Gather and analyze libraries related to ESP32 + GC9A01, GC9A01 + LVGL, and analog clock implementations.
   - Verify the suitability of libraries: LVGL, TFT_eSPI, LovyanGFX, Arduino_GFX.

3. **Architekturvorschlag**:
   - Propose a project structure that accommodates shared components for CAN, OBD, and vehicle state.
   - Define GPIO pin assignments for the GC9A01 display.

4. **Erstellung der Konfigurationsdateien**:
   - Create `DisplayPins.h` for managing GPIO pin configurations sensibly.

5. **Implementierung eines minimalen Testprogramms**:
   - Establish communication with the display, verify basic functionality with simple graphics.

6. **Implementierung der Uhr**:
   - Display the historical clock background and implement rotating hands for hours, minutes, and seconds.

7. **Testmodus einbauen**:
   - Create a mode to test the clock hands independently of the actual time for validation of positioning.

## Detailed Steps

### 1. Bestandsanalyse
- Review the existing project under `motor-node/` for:
  - CAN-bus implementations
  - ELM327/OBD2 functionality
  - Asset availability for graphical backgrounds and instrumentation
- Identify any conflicts with GPIO pin assignments.

### 2. Recherche
- Search for GitHub repositories or articles related to:
  - **ESP32 + GC9A01**  
    - [GC9A01A Round Display Adafruit Example](https://esp32.co.uk/gc9a01a-round-display-adafruit-example/)  
    - [Arduino GC9A01 Display Driver Demo](https://github.com/carlfriess/GC9A01_demo)  
  - **GC9A01 + LVGL**  
    - [Using GC9A01 Round LCD Modules - DroneBot Workshop](https://dronebotworkshop.com/gc9a01/)  
    - Additional examples on using LVGL with the GC9A01.
  - **Analoge Uhr mit LVGL**  
    - [ESP32 TFT with LVGL: Digital Clock with Time and Date](https://randomnerdtutorials.com/esp32-tft-lvgl-digital-clock/)  
  - **Rotierende Zeiger/Bildobjekte**  
    - [How to create an analog watch with LVGL](https://circuitdigest.com/tutorial/getting-started-with-arduino-lvgl)

### 3. Architekturvorschlag
- Proposed directory structure:
  ```text
  workspace/
  ├── motor-node/
  ├── headunit/
  │   ├── src/
  │   │   ├── main.cpp
  │   │   ├── display/
  │   │   │   ├── DisplayManager.cpp
  │   │   │   └── DisplayConfig.h
  │   │   ├── ui/
  │   │   │   ├── ClockScreen.cpp
  │   │   │   └── UiManager.h
  │   │   ├── time/
  │   │   │   ├── ClockModel.cpp
  │   │   │   └── TimeProvider.h
  │   │   └── hardware/
  │   │       └── Pins.h
  │   └── assets/
  └── shared/
      ├── can/
      ├── obd/
      ├── protocol/
      └── common/
  ```

### 4. Erstellung der Konfigurationsdateien
- Create `Pins.h` to manage the following GPIO assignments:
  ```cpp
  const int SCL_PIN = 18;  // SPI Clock
  const int SDA_PIN = 23;  // SPI Data (MOSI)
  const int CS_PIN = 5;    // Chip Select
  const int DC_PIN = 27;   // Data/Command
  const int RST_PIN = 33;  // Reset
  ```

### 5. Implementierung eines minimalen Testprogramms
- Load the display with a basic color fill to ensure proper initialization and check connectivity.

### 6. Implementierung der Uhr
- Reuse the historical clock graphic, implementing the display of the clock hands with care to maintain rotational integrity.
- Use LVGL transformations to handle rotations correctly.

### 7. Testmodus einbauen
- Implement a test configuration to manipulate hands for defined testing times, allowing precise validation.

### Documentation of Findings
I will document all libraries and example projects found in detail, providing reasons for their selection based on maintainability, usability, and connection to existing components.

Once this plan is fully detailed, I will outline the specific files to be created or modified and strategies for inter-project data sharing to promptly execute the tasks in a follow-up action.