#include <Arduino.h>
#include <IRremote.hpp>
#include <SPIFFS.h>
#include <Preferences.h> // For nonvolatile storage on ESP32

#define IR_RECEIVE_PIN 15  // Pin for the IR receiver

// --- Global Variables ---
unsigned long timestampStart = 0;    // Start time for timestamps
unsigned long actionCount = 0;       // To keep track of log file numbering

String lastButton = "";              // Stores the last button name to detect holds
unsigned long lastButtonTimestamp = 0;

// This flag is used to ensure that a held button is logged only once per hold event.
bool holdLogged = false;

String currentFileName = "";         // Current log file name for the active session
bool sessionActive = false;          // True when a session is active (i.e. between power presses)

// Mode flag: if true, we are in IR Mode; if false, in File Management mode.
bool irMode = true;

// For file management (listing/deleting files), we use these globals:
String fileList[50];                 // Assuming a max of 50 files
int fileCount = 0;
String fileName = "";                // Used in deleteAllFiles (per your original algorithm)

// Instead of a constant, use a mutable global variable so it can be changed at runtime.
String logFileBase = "/premiere_log";

// Create a Preferences object for nonvolatile storage.
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

// Initialize SPIFFS
void initFileSystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount file system");
    while (1);
  }
  Serial.println("SPIFFS mounted successfully");
}

// Write a line to the current log file (append mode)
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

// Log a command with a timestamp into the active session file
void logCommand(String buttonName) {
  unsigned long currentTime = millis() - timestampStart; // in ms
  String commandStr = "app.project.activeSequence.videoTracks[1].insertClip(\"" +
                      buttonName + ".mp4\", " +
                      String(currentTime / 1000.0, 3) + ");";
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

// List all stored files with numbering
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

// Delete all files using the original algorithm
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

// Handle IR button presses and session logic with improved hold detection
void handleButtonPress(uint32_t command) {
  String buttonName = "";
  Serial.println(command);
  
  // Map IR command values to button names
  switch ((int)command) {
    case 33: buttonName = "power"; break;
    case 25: buttonName = "ok"; break;
    case 24: buttonName = "right"; break;
    case 22: buttonName = "down"; break;
    case 23: buttonName = "left"; break;
    case 21: buttonName = "up"; break;
    case 71: buttonName = "home"; break;
    case 16: buttonName = "settings"; break;
    case 72: buttonName = "back"; break;
    case 50: buttonName = "tv"; break;
    default: buttonName = ""; break; // Unknown command
  }
  
  if (buttonName == "") return; // Ignore unknown commands

  // --- Improved Hold Detection ---
  bool isRepeat = false;
  // If the IR library supports a repeat flag, use it.
  #ifdef IRDATA_FLAGS_IS_REPEAT
    isRepeat = (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT);
  #else
    // Fallback: if the same button is pressed within a defined threshold, consider it a hold.
    const unsigned long holdThreshold = 700; // Adjust as needed (in milliseconds)
    isRepeat = (buttonName == lastButton && (millis() - lastButtonTimestamp) < holdThreshold);
  #endif

  if (isRepeat) {
    if (buttonName != "power") {
      // Log the held button only once per hold event.
      if (!holdLogged) {
        buttonName = buttonName + "_hold";
        holdLogged = true;
      } else {
        // Already logged the hold event; do not log duplicate hold events.
        return;
      }
    }
  } else {
    // Reset the flag if this is not a repeat event.
    holdLogged = false;
  }
  // --- End Improved Hold Detection ---

  // Session handling for the power button
  if (buttonName == "power") {
    if (!sessionActive) {
      // --- Start a new session ---
      currentFileName = logFileBase + String(actionCount) + ".txt";
      sessionActive = true;
      timestampStart = millis(); // Reset timestamp for new session
      Serial.println("Session started: " + currentFileName);
      logCommand(buttonName); // Log the power press that starts the session
    } else {
      // --- End the current session ---
      logCommand(buttonName); // Log the power press that ends the session
      sendFileOverSerial(currentFileName.c_str()); // Optionally send file over Serial
      sessionActive = false;
      actionCount++; // Increment for the next session
      Serial.println("Session ended: " + currentFileName);
      Serial.println("Type 'menu' to return to the main menu, or press power to start a new session.");
    }
  } else {
    // Log non-power commands only when a session is active.
    if (sessionActive) {
      logCommand(buttonName);
    } else {
      Serial.println("Ignoring command (" + buttonName + ") outside an active session.");
    }
  }
  
  lastButton = buttonName;
  lastButtonTimestamp = millis();
}

// Handle Serial commands in File Management mode
void handleSerialCommand(String command) {
  command.trim(); // Remove any extra whitespace
  
  // In File Management mode, typing "menu" returns you to the main menu.
  if (command == "menu") {
    selectMode();
    return;
  }
  
  // Also, you can change the log file base using "setbase <new_base>".
  if (command.startsWith("setbase ")) {
    String newBase = command.substring(8);
    newBase.trim();
    if (newBase.length() > 0) {
      logFileBase = newBase;
      // Save the new base to nonvolatile storage.
      preferences.putString("logBase", logFileBase);
      Serial.println("Log file base changed to: " + logFileBase);
    } else {
      Serial.println("Invalid base name.");
    }
    return;
  }
  
  // Delete commands: either "delete" for all files or "delete <num>" for a specific file.
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
      // Refresh the file list
      listStoredFiles();
    } else {
      Serial.println("Invalid file number.");
    }
    return;
  }
  
  if (command == "list") {
    listStoredFiles();
  } 
  else if (command.startsWith("send ")) {
    String argument = command.substring(5); // Everything after "send "
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
  } 
  else {
    Serial.println("Unknown command. Available commands:");
    Serial.println("  list                 - List all stored files with numbers");
    Serial.println("  delete               - Delete all stored files");
    Serial.println("  delete <num>         - Delete a specific file by number");
    Serial.println("  send <num>           - Send a specific file over Serial by number");
    Serial.println("  send all             - Send all files over Serial");
    Serial.println("  setbase <new_base>   - Change the log file base");
    Serial.println("  menu                 - Exit File Management mode and go to the main menu");
  }
}

// Display the main menu and allow mode selection
void selectMode() {
  Serial.println();
  Serial.println("========== MENU ==========");
  Serial.println("Select Mode:");
  Serial.println("1 - IR Mode (Log IR signals)");
  Serial.println("2 - File Management Mode");
  Serial.println("Enter your choice:");
  
  // Wait for input
  while (!Serial.available()) {
    delay(100);
  }
  char choice = Serial.read();
  // Clear any leftover input
  while (Serial.available()) { Serial.read(); }
  
  if (choice == '1') {
    irMode = true;
    Serial.println("IR Mode selected. Logging IR commands...");
  } 
  else if (choice == '2') {
    irMode = false;
    Serial.println("File Management Mode selected.");
    // Display the current log file base so you know what it is.
    Serial.println("Current log file base is: " + logFileBase);
    Serial.println("Available commands:");
    Serial.println("  list, delete, delete <num>, send <num>, send all, setbase <new_base>, menu");
    Serial.println("Type 'menu' to exit File Management Mode and go to the main menu.");
    listStoredFiles();
  } 
  else {
    Serial.println("Invalid selection. Defaulting to IR Mode.");
    irMode = true;
  }
}

// --- Arduino Setup and Loop Functions ---

void setup() {
  Serial.begin(115200);
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK); // Initialize the IR receiver
  initFileSystem(); // Mount SPIFFS
  
  // Open preferences in read/write mode.
  preferences.begin("my-app", false);
  // Load the stored log file base or use the default.
  logFileBase = preferences.getString("logBase", "/premiere_log");
  Serial.println("Log file base loaded: " + logFileBase);
  
  selectMode();     // Show the menu to choose mode
}

void loop() {
  // In IR Mode...
  if (irMode) {
    // In IR mode, when no session is active you can type "menu" to return to the main menu.
    if (!sessionActive && Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input == "menu") {
        selectMode();
        return;
      }
    }
    if (IrReceiver.decode()) {
      handleButtonPress(IrReceiver.decodedIRData.command);
      delay(500); // Maintain a delay so that duplicate logs are minimized.
      IrReceiver.resume();
    }
  } 
  // In File Management Mode...
  else {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      handleSerialCommand(input);
    }
  }
}
