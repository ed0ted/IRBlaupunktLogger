#include <Arduino.h>
#include <IRremote.hpp>
#include <SPIFFS.h>
#include <Preferences.h>  // For nonvolatile storage on ESP32

#define IR_RECEIVE_PIN 15  // Pin for the IR receiver

// --- Global Variables ---
unsigned long timestampStart = 0;    // Start time for the current session (ms)
  
String lastButton = "";              // For detecting holds
unsigned long lastButtonTimestamp = 0; 

// This flag ensures a held button is logged only once per hold event.
bool holdLogged = false;

String currentFileName = "";         // File name of the active session (user provided)
bool sessionActive = false;          // True when a session is recording
bool awaitingSessionName = false;    // True when waiting for a new session name

// Mode flag: true = IR Mode, false = File Management Mode.
bool irMode = true;

// For file management mode:
String fileList[50];                 // Maximum of 50 files
int fileCount = 0;
String fileName = "";                // Used in deletion routines

// Although not used for session naming now, we still keep logFileBase for file management.
String logFileBase = "/premiere_log";

// Globals for Premiere Pro track selection:
unsigned long lastClipTime = 0;      // Time (ms relative to session start) of last logged clip
int currentTrackIndex = 1;           // The track index for the next clip

// Create a Preferences object for persistent storage.
Preferences preferences;

// --- Function Prototypes ---
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
void setup();
void loop();

// --- Function Definitions ---

// Initialize SPIFFS.
void initFileSystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount SPIFFS");
    while (1);
  }
  Serial.println("SPIFFS mounted successfully");
}

// Append a line to the active session file.
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

// Log a command (clip insertion) with timestamp and track selection.
// (Power commands are reserved for ending the session.)
void logCommand(String buttonName) {
  unsigned long clipTime = millis() - timestampStart;  // Time (ms) relative to session start

  // For non-session-control commands, update track selection.
  if (buttonName != "power" && buttonName != "power_hold") {
    if ((clipTime - lastClipTime) < 1000) {
      currentTrackIndex++;
    } else {
      currentTrackIndex = 1;
    }
    lastClipTime = clipTime;
  }
  
  String commandStr = "app.project.activeSequence.videoTracks[" + String(currentTrackIndex) +
                        "].insertClip(\"" + buttonName + ".mp4\", " +
                        String(clipTime / 1000.0, 3) + ");";
  Serial.println(commandStr);
  writeToFile(commandStr);
}

// Send a file over Serial.
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

// List all stored files with numbering.
void listStoredFiles() {
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  fileCount = 0;  // Reset counter
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

// Delete all files.
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

// Send all files over Serial.
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

// Process an IR remote command (except power).
void handleButtonPress(uint32_t command) {
  String buttonName = "";
  // Map IR command values (excluding the power command, which is used for ending a session)
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
  
  // --- Improved Hold Detection ---
  bool isRepeat = false;
  #ifdef IRDATA_FLAGS_IS_REPEAT
    isRepeat = (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT);
  #else
    const unsigned long holdThreshold = 700; // ms
    isRepeat = (buttonName == lastButton && (millis() - lastButtonTimestamp) < holdThreshold);
  #endif
  if (isRepeat) {
    if (!holdLogged) {
      buttonName += "_hold";
      holdLogged = true;
    } else {
      return; // Already logged this hold event.
    }
  } else {
    holdLogged = false;
  }
  // --- End Hold Detection ---
  
  logCommand(buttonName);
  lastButton = buttonName;
  lastButtonTimestamp = millis();
}

// Process serial commands in File Management mode.
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
    Serial.println("  menu                 - Exit File Management Mode and return to the main menu");
  }
}

// Display the main menu and allow mode selection.
void selectMode() {
  Serial.println();
  Serial.println("========== MENU ==========");
  Serial.println("Select Mode:");
  Serial.println("1 - IR Mode (Record IR signals)");
  Serial.println("2 - File Management Mode");
  Serial.println("Enter your choice:");
  
  while (!Serial.available()) {
    delay(100);
  }
  char choice = Serial.read();
  while (Serial.available()) { Serial.read(); } // Clear leftover input
  
  if (choice == '1') {
    irMode = true;
    Serial.println("IR Mode selected.");
  } else if (choice == '2') {
    irMode = false;
    Serial.println("File Management Mode selected.");
    Serial.println("Current log file base is: " + logFileBase);
    Serial.println("Available commands:");
    Serial.println("  list, delete, delete <num>, send <num>, send all, setbase <new_base>, menu");
    Serial.println("Type 'menu' to return to the main menu.");
    listStoredFiles();
  } else {
    Serial.println("Invalid selection. Defaulting to IR Mode.");
    irMode = true;
  }
}

// --- Arduino Setup and Loop Functions ---
void setup() {
  Serial.begin(115200);
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK); // Initialize IR receiver
  initFileSystem();  // Mount SPIFFS
  
  // Open preferences and load stored log file base.
  preferences.begin("my-app", false);
  logFileBase = preferences.getString("logBase", "/premiere_log");
  Serial.println("Log file base loaded: " + logFileBase);
  
  selectMode();  // Show main menu.
}

void loop() {
  // ----- IR Mode: Recording IR signals -----
  if (irMode) {
    // If no session is active, prompt for a file name.
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
        // Prepend '/' if missing.
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
        // Flush any pending IR signals and wait a bit.
        while (IrReceiver.decode()) { IrReceiver.resume(); }
        delay(500);
      }
    } else {
      // Session is active—process IR remote commands.
      if (IrReceiver.decode()) {
        uint32_t cmd = IrReceiver.decodedIRData.command;
        // If the power button (assumed value 33) is pressed, end the session.
        if ((int)cmd == 33) {
          Serial.println("Session ended: " + currentFileName);
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
          } else {  // assume "n" or any other input means delete
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
          while ((millis() - startTime) < 3000) {  // Wait up to 3 seconds for input.
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
          return;
        } else {
          // Process any other IR command.
          handleButtonPress(cmd);
        }
        delay(500);  // Delay to minimize duplicate logging.
        IrReceiver.resume();
      }
    }
  }
  // ----- File Management Mode -----
  else {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      handleSerialCommand(input);
    }
  }
}
