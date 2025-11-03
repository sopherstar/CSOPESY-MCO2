#include <iostream>
using namespace std;
#include <thread>
#include <atomic>
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
#include <unordered_map>

// shared state
bool is_initialized = false;
size_t display_width = 100;         //TO-DO : do we need this
std::queue<char> key_buffer;    //TO-DO : do we need this
std::mutex key_buffer_mutex;    //TO-DO : do we need this
std::atomic<bool> is_running{true};

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

enum class InstrType { PRINT, DECLARE, ADD, SUBTRACT, SLEEP, FOR_ };

struct Instruction {
    InstrType type{};
    // For PRINT
    std::string msg;

    // For DECLARE
    std::string var;
    uint16_t value{0};

    // For ADD/SUBTRACT
    std::string var1, var2, var3; // allow var or literal in var2/var3
    bool var2_is_literal{false};
    bool var3_is_literal{false};
    uint16_t lit2{0}, lit3{0};

    // For SLEEP
    uint8_t sleep_ticks{0};

    // For FOR
    std::vector<Instruction> body;
    uint32_t repeats{0};
};

struct PseudoProcess {
    int pid{0};
    std::string name;
    std::chrono::steady_clock::time_point start_time;
    bool running{false};
    bool finished{false};
    size_t pc{0};
    uint8_t sleep_left{0};
    std::vector<Instruction> program;
    std::unordered_map<std::string, uint16_t> mem;
};

std::vector<PseudoProcess> g_processes;
std::mutex g_processes_mtx;
int g_next_pid = 1;

static inline uint16_t clamp_u16(int32_t x) {
    if (x < 0) return 0;
    if (x > 0xFFFF) return 0xFFFF;
    return static_cast<uint16_t>(x);
}

// Resolve "var or literal" into a value; auto-declare vars to 0
static uint16_t read_val(PseudoProcess& p, const std::string& name, bool is_lit, uint16_t lit) {
    if (is_lit) return lit;
    auto it = p.mem.find(name);
    if (it == p.mem.end()) {
        p.mem[name] = 0;
        return 0;
    }
    return it->second;
}

// Return uptime in ms
static long long uptime_ms(const PseudoProcess& pr) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - pr.start_time).count();
}

static std::vector<Instruction> make_default_program(const std::string& pname) {
    std::vector<Instruction> prog;

    Instruction d; d.type = InstrType::DECLARE; d.var = "x"; d.value = 0; prog.push_back(d);

    Instruction loop; loop.type = InstrType::FOR_; loop.repeats = 3;

    Instruction add; add.type = InstrType::ADD;
    add.var1 = "x"; add.var2 = "x"; add.var3_is_literal = true; add.lit3 = 1;
    loop.body.push_back(add);

    Instruction sl; sl.type = InstrType::SLEEP; sl.sleep_ticks = 1;
    loop.body.push_back(sl);

    Instruction pr; pr.type = InstrType::PRINT;
    pr.msg = "Hello world from " + pname + "!";
    loop.body.push_back(pr);

    prog.push_back(loop);
    return prog;
}


//intialize TO-DO: create this (already done in the command_interpreter_thread)

//screen marquee logic TO-DO: create this

//scheduler start TO-DO: create this

//scheduler stop TO-DO: create this

//report utilization TO-DO: create this

// Command interpreter
void command_interpreter_thread(string input) {
    vector<string> tokens = tokenize_input(input);
    if (tokens.empty()) return;

    const string& cmd = tokens[0];

	// exit works anytime
    if (cmd == "exit") {
        cout << "\nExiting...\n";
        cout << "\nProgram Exited.\n";
        is_running = false;
        return;
    }

    // initialize before anything else
    if (cmd == "initialize") {
        if (is_initialized) {
            cout << "Already initialized.\n";
            return;
        }

        is_initialized = true;
        cout << "System initialized.\n";
        return;
    }

    // rejects all other commands if not initialized
    if (!is_initialized) {
        cout << "Error: system not initialized. Run \"initialize\" or \"exit\".\n";
        return;
    }

    // commands below require an initialized system
    if (cmd == "help") {
        cout << "List of commands:\n";
        cout << "\"help\" - displays the commands and their descriptions\n";
        cout << "\"initialize\" - initialize the processor configuration (must be run first)\n";
        cout << "\"exit\" - terminates the console\n";
        cout << "\"screen -s <program name>\" - creates a new process\n";
        cout << "\"screen -ls\" - lists all running processes\n";
        cout << "\"scheduler-start\" - start the scheduler which continuously generates a batch of dummy processes for the CPU scheduler\n";
        cout << "\"scheduler-stop\" - stop the scheduler/generating dummy processes \n";
        cout << "\"report-util\" - generate of CPU utilization report\n";
    }
    else if (cmd == "screen") {
        if (tokens.size() == 1) {
        cout << "Usage:\n"
             << "  screen -s <process name>   Create a new process\n"
             << "  screen -ls                 List running processes\n";
        return;
    	}

    	if (tokens[1] == "-ls") {
        	std::lock_guard<std::mutex> lk(g_processes_mtx);
        	if (g_processes.empty()) {
            	cout << "No processes found.\n";
            	return;
        	}
        	cout << "PID\tSTATE\tUPTIME(ms)\tNAME\n";
        	for (const auto& p : g_processes) {
            	const char* st = p.finished ? "FINISHED" : (p.running ? "RUNNING" : "READY");
            	cout << p.pid << '\t' << st << '\t' << uptime_ms(p) << '\t' << p.name << '\n';
        	}
        	return;
    	}

    	if (tokens[1] == "-s") {
        	if (tokens.size() < 3) {
            	cout << "Error: missing <process name>.\n";
            	return;
        	}
        
        	std::ostringstream oss;
        	for (size_t i = 2; i < tokens.size(); ++i) {
            	if (i > 2) oss << ' ';
            	oss << tokens[i];
        	}
        	std::string pname = oss.str();

        	PseudoProcess proc;
        	proc.pid = g_next_pid++;
        	proc.name = pname;
        	proc.start_time = std::chrono::steady_clock::now();
        	proc.running = false;
        	proc.program = make_default_program(pname);

        	{
            	std::lock_guard<std::mutex> lk(g_processes_mtx);
            	g_processes.push_back(std::move(proc));
        	}

        	cout << "Started process \"" << pname << "\" with PID " << (g_next_pid - 1) << ".\n";
        	return;
    	}

    	cout << "Unknown 'screen' option. Try: screen -s <name>  or  screen -ls\n";
    }
    else if (cmd == "scheduler-start") {
        // TODO
    }
    else if (cmd == "scheduler-stop") {
        // TODO
    }
    else if (cmd == "report-util") {
        // TODO
    }
    else {
        cout << "Unknown command. Type \"help\".\n";
    }
}

// Display handler – handles the display for the command interpreter and marquee logic TO-DO: fix this
void display_handler_thread() {
    //thread marqueeThread(marquee_logic_thread);       TO-DO: fix this
    //marqueeThread.detach();

    cout << "\nGroup developer:" <<endl;
    cout << "CISNEROS, JOHN MAVERICK ZARAGOSA\nILUSTRE, SOPHIA MACAPINLAC\nJOCSON, VINCE MIGUEL\nVERGARA, ROYCE AARON ADAM\n" <<endl;

    cout << "Version date:\n" <<endl;

    cout << "Type 'help' to see the list of commands.\n\n";
    //layout and design of the console
}

// Keyboard handler – handles keyboard buffering and polling
void keyboard_handler_thread() {
    while (is_running) {
        if (_kbhit()) {
            char ch = _getch();
            std::lock_guard<std::mutex> lock(key_buffer_mutex);
            key_buffer.push(ch); // Buffer the key press
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}


int main() {
    int cpuCycles = 0;
    string input;
    bool is_running = true;

    thread displayThread(display_handler_thread);
    thread keyboardThread(keyboard_handler_thread);
	
	cout << "Command> ";
    while(is_running){
        {
            std::lock_guard<std::mutex> lock(key_buffer_mutex);
            while (!key_buffer.empty()) {
                char ch = key_buffer.front();
                key_buffer.pop();
                //std::cout << "\nKey pressed: " << ch << std::endl;
                if (ch == '\r') { // enter
                    cout << endl;
                    thread commandThread(command_interpreter_thread, input);
                    commandThread.join();
                    input.clear();
                    cout << "Command> ";
                } else if (ch == '\b') { // backspace
                    if (!input.empty()) {
                        input.pop_back();
                        cout << "\b \b";
                    }
                } else if (isprint(ch)) {
                    input += ch;
                    cout << ch;
                }
            }
        }
        this_thread::sleep_for(chrono::milliseconds(50));
    }


    displayThread.join();
    keyboardThread.join();
    return 0;
}