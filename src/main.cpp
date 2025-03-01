#include <Arduino.h>
#include <IRremote.hpp>
#include <SPIFFS.h>
#include <Preferences.h>
#include <BleKeyboard.h>

// =========== IR Receiver Pin ===========
#define IR_RECEIVE_PIN 15

// =========== Global Variables (IR & File) ===========
unsigned long timestampStart = 0;     // Session start time in ms
String lastButton = "";
unsigned long lastButtonTimestamp = 0;
bool holdLogged = false;
String currentFileName = "";
bool sessionActive = false;
bool awaitingSessionName = false;

String fileList[50];                 // Up to 50 files
int fileCount = 0;
String fileName = "";
String logFileBase = "/premiere_log"; // Base for file naming

unsigned long lastClipTime = 0;      // Time of last logged clip
int currentTrackIndex = 1;           // Track index for next clip

Preferences preferences;

// =========== Global Variables (Mode & BLE) ===========
// 1 = IR Mode, 2 = File Management, 3 = BLE Connect/Pair
int currentMode = 0;  

// Create a BLE Keyboard instance 
BleKeyboard bleKeyboard("ESP32 Media Keyboard", "MyCompany", 100);

// =========== Function Prototypes ===========
void initFileSystem();
void writeToFile(String line);
void logCommand(String buttonName);
void sendFileOverSerial(const char *fileNameParam);
void listStoredFiles();
void deleteAllFiles();
void sendAllFilesOverSerial();
void handleButtonPress(uint32_t command);
void handleSerialCommand(String command);
void selectMode();
void sendVolumeUp();
void irModeLoop();
void bleMode();  
void setup();
void loop();

// =========== File/IR Management Functions ===========

// Initialize SPIFFS
void initFileSystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount SPIFFS");
    while (1);
  }
  Serial.println("SPIFFS mounted successfully");
}

// Write a line to the active session file
void writeToFile(String line) {
  if (currentFileName == "") {
    Serial.println("No active session file.");
    return;
  }
  File file = SPIFFS.open(currentFileName, FILE_APPEND);
  if (file) {
    file.println(line);
    file.close();
  } else {
    Serial.println("Failed to open file for writing: " + currentFileName);
  }
}

// Log a command with timestamp + track selection
void logCommand(String buttonName) {
  unsigned long clipTime = millis() - timestampStart;
  // If clip is inserted <1s after last clip, increment track
  if ((clipTime - lastClipTime) < 1000) {
    currentTrackIndex++;
  } else {
    currentTrackIndex = 1;
  }
  lastClipTime = clipTime;

  String commandStr = "app.project.activeSequence.videoTracks[" + String(currentTrackIndex) +
                      "].insertClip(\"" + buttonName + ".mp4\", " +
                      String(clipTime / 1000.0, 3) + ");";
  Serial.println(commandStr);
  writeToFile(commandStr);
}

// Send a file over Serial
void sendFileOverSerial(const char *fileNameParam) {
  Serial.print("Sending: ");
  Serial.println(fileNameParam);
  File file = SPIFFS.open(fileNameParam, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }
  Serial.println("START_FILE_TRANSFER:" + String(fileNameParam));
  while (file.available()) {
    Serial.write(file.read());
  }
  Serial.println("\nEND_FILE_TRANSFER");
  file.close();
}

// List all stored files
void listStoredFiles() {
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  fileCount = 0;
  while (file && fileCount < 50) {
    fileList[fileCount] = file.path();
    Serial.printf("[%d] %s\n", fileCount + 1, file.path());
    file = root.openNextFile();
    fileCount++;
  }
  if (fileCount == 0) {
    Serial.println("No files found.");
  }
}

// Delete all files
void deleteAllFiles() {
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    fileName = "/";
    fileName.concat(file.name());
    SPIFFS.remove(fileName);
    file = root.openNextFile();
  }
  Serial.println("All files deleted.");
  fileName = "";
}

// Send all files over Serial
void sendAllFilesOverSerial() {
  if (fileCount == 0) {
    Serial.println("No files to send.");
    return;
  }
  Serial.println("START_ALL_FILE_TRANSFER");
  for (int i = 0; i < fileCount; i++) {
    sendFileOverSerial(fileList[i].c_str());
  }
  Serial.println("END_ALL_FILE_TRANSFER");
}

// Handle IR remote commands (except end-of-session)
void handleButtonPress(uint32_t command) {
  String buttonName = "";
  switch ((int)command) {
    case 25: buttonName = "ok"; break;
    case 24: buttonName = "right"; break;
    case 22: buttonName = "down"; break;
    case 23: buttonName = "left"; break;
    case 21: buttonName = "up"; break;
    case 71: buttonName = "home"; break;
    case 16: buttonName = "settings"; break;
    case 72: buttonName = "back"; break;
    case 50: buttonName = "tv"; break;
    default: buttonName = ""; break;
  }
  if (buttonName == "") return;
  
  bool isRepeat = false;
  #ifdef IRDATA_FLAGS_IS_REPEAT
    isRepeat = (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT);
  #else
    const unsigned long holdThreshold = 700;
    isRepeat = (buttonName == lastButton && (millis() - lastButtonTimestamp) < holdThreshold);
  #endif
  if (isRepeat) {
    if (!holdLogged) {
      buttonName += "_hold";
      holdLogged = true;
    } else {
      return;
    }
  } else {
    holdLogged = false;
  }
  logCommand(buttonName);
  lastButton = buttonName;
  lastButtonTimestamp = millis();
}

// Handle serial commands in File Management mode
void handleSerialCommand(String command) {
  command.trim();
  if (command == "menu") {
    selectMode();
    return;
  }
  if (command.startsWith("setbase ")) {
    String newBase = command.substring(8);
    newBase.trim();
    if (newBase.length() > 0) {
      logFileBase = newBase;
      preferences.putString("logBase", logFileBase);
      Serial.println("Log file base changed to: " + logFileBase);
    } else {
      Serial.println("Invalid base name.");
    }
    return;
  }
  if (command == "delete") {
    deleteAllFiles();
    return;
  } else if (command.startsWith("delete ")) {
    String argument = command.substring(7);
    argument.trim();
    int fileIndex = argument.toInt();
    if (fileIndex > 0 && fileIndex <= fileCount) {
      String fileToDelete = fileList[fileIndex - 1];
      if (SPIFFS.remove(fileToDelete)) {
        Serial.println("Deleted file: " + fileToDelete);
      } else {
        Serial.println("Failed to delete file: " + fileToDelete);
      }
      listStoredFiles();
    } else {
      Serial.println("Invalid file number.");
    }
    return;
  }
  if (command == "list") {
    listStoredFiles();
  } else if (command.startsWith("send ")) {
    String argument = command.substring(5);
    if (argument == "all") {
      sendAllFilesOverSerial();
    } else {
      int fileIndex = argument.toInt();
      if (fileIndex > 0 && fileIndex <= fileCount) {
        sendFileOverSerial(fileList[fileIndex - 1].c_str());
      } else {
        Serial.println("Invalid file number.");
      }
    }
  } else {
    Serial.println("Unknown command. Available commands:");
    Serial.println("  list                 - List all stored files with numbers");
    Serial.println("  delete               - Delete all stored files");
    Serial.println("  delete <num>         - Delete a specific file by number");
    Serial.println("  send <num>           - Send a specific file over Serial by number");
    Serial.println("  send all             - Send all files over Serial");
    Serial.println("  setbase <new_base>   - Change the log file base");
    Serial.println("  menu                 - Return to the main menu");
  }
}

// =========== Menu Selection ===========

void selectMode() {
  Serial.println();
  Serial.println("========== MENU ==========");
  Serial.println("Select Mode:");
  Serial.println("1 - IR Mode (Record IR signals)");
  Serial.println("2 - File Management Mode");
  Serial.println("3 - BLE Connect/Pair");
  Serial.println("Enter your choice:");
  
  while (!Serial.available()) {
    delay(100);
  }
  char choice = Serial.read();
  while (Serial.available()) { Serial.read(); }
  
  if (choice == '1') {
    currentMode = 1;
    Serial.println("IR Mode selected.");
  } else if (choice == '2') {
    currentMode = 2;
    Serial.println("File Management Mode selected.");
    Serial.println("Current log file base is: " + logFileBase);
    Serial.println("Available commands:");
    Serial.println("  list, delete, delete <num>, send <num>, send all, setbase <new_base>, menu");
    Serial.println("Type 'menu' to return to main menu.");
    listStoredFiles();
  } else if (choice == '3') {
    currentMode = 3;
    Serial.println("BLE Connect/Pair selected.");
  } else {
    Serial.println("Invalid selection. Defaulting to IR Mode.");
    currentMode = 1;
  }
}

// =========== BLE Keyboard Functions ===========

// Send a Volume Up keypress
void sendVolumeUp() {
  if (bleKeyboard.isConnected()) {
    Serial.println("Sending Volume Up...");
    bleKeyboard.press(KEY_MEDIA_VOLUME_UP);
    delay(100);
    bleKeyboard.release(KEY_MEDIA_VOLUME_UP);
    Serial.println("Volume Up sent.");
  } else {
    Serial.println("BLE keyboard not connected; cannot send Volume Up.");
  }
}

// The BLE Connect/Pair mode (option 3)
void bleMode() {
  if (!bleKeyboard.isConnected()) {
    bleKeyboard.begin();
    Serial.println("BLE Keyboard started. Waiting for iOS to connect...");
  }
  Serial.println("Type 'menu' to return to main menu.");
  
  // Wait in a loop until user types 'menu'
  while (true) {
    if (bleKeyboard.isConnected()) {
      preferences.putBool("paired", true);
      Serial.println("BLE keyboard is connected to iOS!");
    }
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd.equalsIgnoreCase("menu")) {
        bleKeyboard.end();
        currentMode = 0;
        return;
      }
    }
    delay(100);
  }
}

// =========== IR Mode Loop ===========

void irModeLoop() {
  // If no session is active, prompt for a file name
  if (!sessionActive) {
    if (!awaitingSessionName) {
      Serial.println("Enter file name for new session (or type 'menu' to return to menu):");
      awaitingSessionName = true;
    }
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.equalsIgnoreCase("menu")) {
        awaitingSessionName = false;
        selectMode();
        return;
      }
      if (input.charAt(0) != '/') {
        input = "/" + input;
      }
      currentFileName = input + ".txt";
      sessionActive = true;
      awaitingSessionName = false;
      timestampStart = millis();
      lastClipTime = 0;
      currentTrackIndex = 1;
      Serial.println("Session started: " + currentFileName);
      // Send Volume Up if BLE is connected
      sendVolumeUp();
      
      // Flush IR signals
      while (IrReceiver.decode()) { IrReceiver.resume(); }
      delay(500);
    }
  } 
  else {
    // Session is active—process IR remote commands and also check for "end" in Serial
    if (IrReceiver.decode()) {
      uint32_t cmd = IrReceiver.decodedIRData.command;
      // Log any IR command except end-of-session
      handleButtonPress(cmd);
      delay(500);
      IrReceiver.resume();
    }

    // Check if user typed "end" in Serial to finish session
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.equalsIgnoreCase("end")) {
        // End the session
        Serial.println("Session ended: " + currentFileName);
        sendVolumeUp(); // Send Volume Up at session end

        Serial.println("Do you want to save the recorded file? (y/n) or type 'menu' to return to main menu");
        while (!Serial.available()) {
          delay(100);
        }
        String decision = Serial.readStringUntil('\n');
        decision.trim();
        if (decision.equalsIgnoreCase("y")) {
          Serial.println("File saved.");
        } else if (decision.equalsIgnoreCase("menu")) {
          if (SPIFFS.remove(currentFileName)) {
            Serial.println("File deleted.");
          } else {
            Serial.println("Error deleting file.");
          }
          sessionActive = false;
          currentFileName = "";
          selectMode();
          return;
        } else {
          // assume "n" or any other input means delete
          if (SPIFFS.remove(currentFileName)) {
            Serial.println("File deleted.");
          } else {
            Serial.println("Error deleting file.");
          }
        }
        sessionActive = false;
        currentFileName = "";
        Serial.println("Type 'menu' to return to main menu, or press Enter to start a new session.");
        unsigned long startTime = millis();
        while ((millis() - startTime) < 3000) {
          if (Serial.available()) {
            String menuDecision = Serial.readStringUntil('\n');
            menuDecision.trim();
            if (menuDecision.equalsIgnoreCase("menu")) {
              selectMode();
              return;
            }
          }
          delay(100);
        }
      }
    }
  }
}

// =========== Setup & Loop ===========

void setup() {
  Serial.begin(115200);
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);
  initFileSystem();
  
  preferences.begin("my-app", false);
  logFileBase = preferences.getString("logBase", "/premiere_log");
  Serial.println("Log file base loaded: " + logFileBase);
  
  selectMode();
}

void loop() {
  if (currentMode == 0) {
    selectMode();
  } else if (currentMode == 1) {
    irModeLoop();
  } else if (currentMode == 2) {
    // File Management
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      handleSerialCommand(input);
    }
  } else if (currentMode == 3) {
    bleMode();
  }
  delay(10);
}
