#include <iostream>
using namespace std;
#include <thread>

#include <string>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <sstream>
#include <vector>
#include <string>
#include <conio.h>
#include <queue>
#include <mutex>

// shared state
bool is_initialized = false;
size_t display_width = 100;         //TO-DO : do we need this
std::queue<char> key_buffer;    //TO-DO : do we need this
std::mutex key_buffer_mutex;    //TO-DO : do we need this

//HELPER FUNCTION
vector<string> tokenize_input(const string& input) {
    vector<string> tokens;
    istringstream iss(input);
    string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}
//intialize TO-DO: create this

//screen marquee logic TO-DO: create this

//scheduler start TO-DO: create this

//scheduler stop TO-DO: create this

//report utilization TO-DO: create this

// Command interpreter
void command_interpreter_thread(string input) {
    vector<string> tokens = tokenize_input(input);

    if (tokens[0] == "initialize"){
        //is_initialized = true; TO-DO: create this
    }
    else if (tokens [0] == "exit"){

    }
    else if (tokens[0] == "screen"){

    }
    else if (tokens[0] == "scheduler-start"){

    }
    else if (tokens[0] == "scheduler-stop"){

    }
    else if (tokens[0] == "report-util"){
        //print to csopesy-log.txt
    }
}

// Display handler – handles the display for the command interpreter and marquee logic TO-DO: fix this
void display_handler_thread() {
    //thread marqueeThread(marquee_logic_thread);       TO-DO: fix this
    //marqueeThread.detach();

    cout << "Group developer:" <<endl;
    cout << "CISNEROS, JOHN MAVERICK ZARAGOSA\nILUSTRE, SOPHIA MACAPINLAC\nJOCSON, VINCE MIGUEL\nVERGARA, ROYCE AARON ADAM\n" <<endl;

    cout << "Version date:\n" <<endl;

    cout << "Type 'help' to see the list of commands.\n\n";
    //layout and design of the console
}

// Keyboard handler – handles keyboard buffering and polling
void keyboard_handler_thread() {
    while (true) {
        if (_kbhit()) {
            char ch = _getch();
            std::lock_guard<std::mutex> lock(key_buffer_mutex);
            key_buffer.push(ch); // Buffer the key press
        }
        this_thread::sleep_for(chrono::milliseconds(50));
    }
}


int main() {
    int cpuCycles = 0;
    bool is_running = true;

    thread displayThread(display_handler_thread);

    while (is_running) {
        cpuCycles++;
    }

    displayThread.join();
    return 0;
}