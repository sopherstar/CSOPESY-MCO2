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
#include <fstream>
#include <algorithm>

// default configuration settings, loaded from config.txt
struct Config {
    int num_cpu = 4;
    std::string scheduler = "rr";
    int quantum_cycles = 5;
    long batch_process_freq = 1;
    long min_ins = 1000;
    long max_ins = 2000;
    long delay_per_exec = 0;
};

// global configuration
Config g_config;

std::atomic<int> g_cpu_cycles{0};
std::queue<int> g_ready_queue;
std::mutex g_ready_queue_mtx;

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

    std::vector<std::string> log; // For PRINT instruction

    // Stack for FOR loops
    struct LoopFrame {
        size_t for_instr_pc; // The index (in p.program) of the FOR_ instruction
        size_t body_pc;      // The current index within the FOR_ body
        uint32_t repeats_left;
    };
    std::vector<LoopFrame> loop_stack;
};

std::vector<PseudoProcess> g_processes;
std::mutex g_processes_mtx;
int g_next_pid = 1;
std::atomic<bool> scheduler_generating{false};
std::thread scheduler;

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

// Scheduler Start
void scheduler_start() {
    // Start generating dummy processes based on CPU ticks (g_cpu_cycles)
    scheduler_generating = true;

    // Ensure batch frequency sensible
    long freq = g_config.batch_process_freq;
    if (freq <= 0) freq = 1;

    // Track last tick used to avoid creating multiple processes for same tick
    long long last_tick = g_cpu_cycles.load();

    while (scheduler_generating && is_running) {
        long long cur = g_cpu_cycles.load();
        if (cur - last_tick >= freq) {
            // time to create a new process
            PseudoProcess proc;
            // Reserve a pid under lock
            {
                std::lock_guard<std::mutex> lk(g_processes_mtx);
                proc.pid = g_next_pid++;
            }

            std::ostringstream pname_ss;
            pname_ss << "auto-" << proc.pid;
            proc.name = pname_ss.str();
            proc.start_time = std::chrono::steady_clock::now();
            proc.running = false;
            proc.program = make_default_program(proc.name);

            {
                std::lock_guard<std::mutex> lk(g_processes_mtx);
                g_processes.push_back(std::move(proc));
            }

            // push into ready queue so CPU cores can pick it up
            {
                std::lock_guard<std::mutex> lk(g_ready_queue_mtx);
                g_ready_queue.push(g_next_pid - 1);
            }

            std::cout << "[scheduler] generated process auto-" << (g_next_pid - 1) << "\n";

            // advance last_tick
            last_tick = cur;
        }

        // Sleep short while to avoid busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    scheduler_generating = false;
}

// Scheduler Stop
void scheduler_stop() {
    // Signal the generator to stop
    scheduler_generating = false;

    // If thread is joinable (we created a non-detached thread), join it to clean up
    if (scheduler.joinable()) {
        try {
            scheduler.join();
        } catch (...) {
            // swallow exceptions to avoid termination; nothing much to do here
        }
    }
}

// Report Utilization
// If out_file is non-empty, the same report is also saved to that file (overwrites existing file).
void report_utilization(const std::string& out_file = "") {
    std::lock_guard<std::mutex> lk(g_processes_mtx);
    std::ostringstream oss;
    if (g_processes.empty()) {
        oss << "No processes found.\n";
    } else {
        oss << "PID\tSTATE\tUPTIME(ms)\tNAME\n";
        for (const auto& p : g_processes) {
            const char* st = p.finished ? "FINISHED" : (p.running ? "RUNNING" : "READY");
            oss << p.pid << '\t' << st << '\t' << uptime_ms(p) << '\t' << p.name << '\n';
        }
    }

    // Print to console
    std::cout << oss.str();

    // Optionally save to file (overwrite)
    if (!out_file.empty()) {
        std::ofstream ofs(out_file, std::ios::out | std::ios::trunc);
        if (ofs.is_open()) {
            ofs << oss.str();
            ofs.close();
            std::cout << "Saved report to '" << out_file << "'.\n";
        } else {
            std::cout << "Error: could not open file '" << out_file << "' for writing.\n";
        }
    }
}

// Enum to signal the result of an instruction
enum class ExecStatus { OK, SLEEP, FINISHED };

// helper function to execute instructions
ExecStatus execute_instruction(PseudoProcess& p, Instruction& instr) {
    
    switch (instr.type) {
        case InstrType::PRINT: {
            // Check for variable printing, e.g., PRINT ("Value from: " +x)
            size_t var_pos = instr.msg.find("+x");
            if (var_pos != std::string::npos && instr.msg.find("\"") < var_pos) {
                 // Found "..." +x
                 std::string base_msg = instr.msg.substr(0, var_pos);
                 // Clean up quotes and " +"
                 base_msg.erase(std::remove(base_msg.begin(), base_msg.end(), '"'), base_msg.end());
                 if (base_msg.size() > 2 && base_msg.substr(base_msg.size() - 2) == " +") {
                    base_msg = base_msg.substr(0, base_msg.size() - 2);
                 }

                 uint16_t val = read_val(p, "x", false, 0); // Spec example hardcodes 'x'
                 p.log.push_back(base_msg + std::to_string(val));
            } else {
                 // Simple print, e.g. "Hello World"
                 std::string clean_msg = instr.msg;
                 clean_msg.erase(std::remove(clean_msg.begin(), clean_msg.end(), '"'), clean_msg.end());
                 p.log.push_back(clean_msg);
            }
            break;
        }

        case InstrType::DECLARE:
            p.mem[instr.var] = instr.value;
            break;

        case InstrType::ADD: {
            uint16_t val2 = read_val(p, instr.var2, instr.var2_is_literal, instr.lit2);
            uint16_t val3 = read_val(p, instr.var3, instr.var3_is_literal, instr.lit3);
            // Auto-declare var1
            read_val(p, instr.var1, false, 0); 
            p.mem[instr.var1] = clamp_u16(val2 + val3);
            break;
        }

        case InstrType::SUBTRACT: {
            uint16_t val2 = read_val(p, instr.var2, instr.var2_is_literal, instr.lit2);
            uint16_t val3 = read_val(p, instr.var3, instr.var3_is_literal, instr.lit3);
            // Auto-declare var1
            read_val(p, instr.var1, false, 0);
            p.mem[instr.var1] = clamp_u16(val2 - val3);
            break;
        }

        case InstrType::SLEEP:
            p.sleep_left = instr.sleep_ticks;
            return ExecStatus::SLEEP; // Signal to scheduler

        case InstrType::FOR_:
            // handled by core function
            break;
    }
    return ExecStatus::OK;
}

// CPU thread function
void cpu_core_function(int core_id) {
    while (is_running) {
        // get process id from ready queue
        int pid_to_run = -1;
        {
            std::lock_guard<std::mutex> lk(g_ready_queue_mtx);
            if (!g_ready_queue.empty()) {
                pid_to_run = g_ready_queue.front();
                g_ready_queue.pop();
            }
        }

        if (pid_to_run == -1) {
            // if no work to do, sleep and try again
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        std::lock_guard<std::mutex> lk(g_processes_mtx);
        
        // find process
        PseudoProcess* p = nullptr;
        for (auto& proc : g_processes) {
            if (proc.pid == pid_to_run) {
                p = &proc;
                break;
            }
        }

        // if process not found or already running/finished, skip
        if (p == nullptr || p->finished || p->running) {
            continue;
        }

        p->running = true;
        
        bool process_finished = false;
        bool process_sleeping = false;
        
        // get quantum
        int quantum = (g_config.scheduler == "rr") ? g_config.quantum_cycles : 1000;

        for (int i = 0; i < quantum; ++i) {
            if (g_config.delay_per_exec > 0) {
                // simulate delay
                std::this_thread::sleep_for(std::chrono::milliseconds(g_config.delay_per_exec));
            }

            // get instruction
            Instruction* instr_to_exec = nullptr;

            if (!p->loop_stack.empty()) {
                // for loop
                auto& loop = p->loop_stack.back();
                Instruction& for_instr = p->program[loop.for_instr_pc];
                if (loop.body_pc >= for_instr.body.size()) {
                    loop.repeats_left--;
                    loop.body_pc = 0;
                    if (loop.repeats_left == 0) {
                        p->loop_stack.pop_back();
                        p->pc = loop.for_instr_pc + 1;
                    }
                    continue; 
                }
                instr_to_exec = &for_instr.body[loop.body_pc];
                loop.body_pc++;
            } else {
                if (p->pc >= p->program.size()) {
                    process_finished = true;
                    break;
                }
                
                instr_to_exec = &p->program[p->pc];

                if (instr_to_exec->type == InstrType::FOR_) {
                    if (instr_to_exec->repeats > 0) {
                        p->loop_stack.push_back({p->pc, 0, instr_to_exec->repeats});
                    }
                    p->pc++;
                    continue; 
                }
                
                p->pc++;
            }

            if (process_finished) break;

            // execute instruction
            ExecStatus status = execute_instruction(*p, *instr_to_exec);
            if (status == ExecStatus::SLEEP) {
                process_sleeping = true;
                break;
            }
        }

        if (process_finished) {
            p->running = false;
            p->finished = true;
        } 
        else if (process_sleeping) {
            // running stays true if process is sleeping
        } 
        else {
            // quantum expired, put back in ready queue
            p->running = false;
            std::lock_guard<std::mutex> lk_ready(g_ready_queue_mtx);
            g_ready_queue.push(p->pid);
        }
    }
}

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

        std::ifstream config_file("config.txt");
        if (!config_file.is_open()) {
            cout << "Error: config.txt not found. Cannot initialize.\n";
            return;
        }

        std::string line;
        std::string key;
        std::string value_str;

        while (std::getline(config_file, line)) {
            std::istringstream iss(line);
            if (!(iss >> key >> value_str)) {
                // skip whitespace
                continue;
            }

            try {
                if (key == "num-cpu") {
                    g_config.num_cpu = std::stoi(value_str);
                } else if (key == "scheduler") {
                    // remove quotes from "rr" or "fcfs"
                    if (value_str.front() == '"') value_str.erase(0, 1);
                    if (value_str.back() == '"') value_str.pop_back();
                    g_config.scheduler = value_str;
                } else if (key == "quantum-cycles") {
                    g_config.quantum_cycles = std::stoi(value_str);
                } else if (key == "batch-process-freq") {
                    g_config.batch_process_freq = std::stol(value_str);
                } else if (key == "min-ins") {
                    g_config.min_ins = std::stol(value_str);
                } else if (key == "max-ins") {
                    g_config.max_ins = std::stol(value_str);
                } else if (key == "delay-per-exec") {
                    g_config.delay_per_exec = std::stol(value_str);
                }
            } catch (const std::exception& e) {
                cout << "Error parsing config line: " << line << "\n";
            }
        }
        config_file.close();

        is_initialized = true;
        cout << "System initialized.\n";

        // show configuration summary
        cout << "  - num-cpu: " << g_config.num_cpu << "\n";
        cout << "  - scheduler: " << g_config.scheduler << "\n";
        cout << "  - quantum-cycles: " << g_config.quantum_cycles << "\n";
        cout << "  - batch-process-freq: " << g_config.batch_process_freq << "\n";
        cout << "  - min-ins: " << g_config.min_ins << "\n";
        cout << "  - max-ins: " << g_config.max_ins << "\n";
        cout << "  - delay-per-exec: " << g_config.delay_per_exec << "\n";
        
        // launch cpu threads
        cout << "Launching " << g_config.num_cpu << " CPU cores...\n";
        for (int i = 0; i < g_config.num_cpu; ++i) {
            std::thread(cpu_core_function, i).detach();
        }
        cout << "CPU cores running.\n";
        
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

            {
                std::lock_guard<std::mutex> lk(g_ready_queue_mtx);
                g_ready_queue.push(g_next_pid - 1);
            }
        	cout << "Started process \"" << pname << "\" with PID " << (g_next_pid - 1) << ".\n";
        	return;
    	}

    	cout << "Unknown 'screen' option. Try: screen -s <name>  or  screen -ls\n";
    }
    else if (cmd == "scheduler-start") {
        if (scheduler_generating) {
            cout << "Scheduler already running.\n";
        } else {
            if (scheduler.joinable()) {
                scheduler.join();
            }
            scheduler = std::thread(scheduler_start);
            cout << "Scheduler started.\n";
        }
    }
    else if (cmd == "scheduler-stop") {
        if (!scheduler_generating) {
            cout << "Scheduler is not running.\n";
        } else {
            scheduler_stop();
            cout << "Scheduler stopped.\n";
        }
    }
    else if (cmd == "report-util") {
        // Print report and save to csopesy-log.txt
        report_utilization("csopesy-log.txt");
    }
    else {
        cout << "Unknown command. Type \"help\".\n";
    }
}

// Scheduler thread
void scheduler_thread() {
    while (is_running) {
        if (is_initialized) {
            g_cpu_cycles++; // system clock tick

            std::vector<int> pids_to_ready;

            // check sleeping processes
            {
                std::lock_guard<std::mutex> lk(g_processes_mtx);
                for (auto& p : g_processes) {
                    // if sleeping (running and sleep_left > 0)
                    if (p.running && p.sleep_left > 0) {
                        p.sleep_left--;
                        if (p.sleep_left == 0) {
                            // process is done sleeping, mark it as ready
                            p.running = false; 
                            pids_to_ready.push_back(p.pid);
                        }
                    }
                }
            } 

            // add new process to ready queue
            if (!pids_to_ready.empty()) {
                std::lock_guard<std::mutex> lk(g_ready_queue_mtx);
                for (int pid : pids_to_ready) {
                    g_ready_queue.push(pid);
                }
            }

            // TODO: Step 4 logic for 'scheduler-start' will go here
            // (checking g_cpu_cycles % g_config.batch_process_freq)

        } // end if(is_initialized)

        // sleep
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Display handler – handles the display for the command interpreter and marquee logic TO-DO: fix this
void display_handler_thread() {
    //thread marqueeThread(marquee_logic_thread);       TO-DO: fix this
    //marqueeThread.detach();

    cout << "\nGroup developer:" <<endl;
    cout << "CISNEROS, JOHN MAVERICK ZARAGOSA\nILUSTRE, SOPHIA MACAPINLAC\nJOCSON, VINCE MIGUEL\nVERGARA, ROYCE AARON ADAM\n" <<endl;

    cout << "Version date: 05/11/2025 1:03pm\n" <<endl;

    cout << "Type 'initialize' to initialize the system.\n\n";
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
    //bool is_running = true;

    thread displayThread(display_handler_thread);
    thread keyboardThread(keyboard_handler_thread);
    thread schedulerThread(scheduler_thread);
	
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
    schedulerThread.join();
    return 0;
}