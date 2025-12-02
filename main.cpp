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
#include <iomanip>
#include <algorithm>
#include <random>


// default configuration settings, loaded from config.txt
struct Config {
    int num_cpu = 4;
    std::string scheduler = "rr";
    int quantum_cycles = 5;
    long batch_process_freq = 1;
    long min_ins = 1000;
    long max_ins = 2000;
    long delay_per_exec = 0;
    
    // MCO2 memory-related config
    long max_overall_mem   = 65536; // bytes, will be overridden by config.txt
    long mem_per_frame     = 64;    // bytes per frame
    long min_mem_per_proc  = 64;    // bytes
    long max_mem_per_proc  = 65536; // bytes
};

// global configuration
Config g_config;

// =======================
// MCO2: Memory structures
// =======================
struct Frame {
    int frame_id{-1};     // index of this frame
    bool used{false};     // is this frame currently allocated?
    int owner_pid{-1};    // which process owns this frame (-1 = none)
    int page_index{-1};   // which virtual page of that process (-1 = none)
};

enum class InstrType { PRINT, DECLARE, ADD, SUBTRACT, SLEEP, FOR_, READ, WRITE};

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
    uint32_t lit2{0}, lit3{0}; // Changed to 32-bit to support large addresses

    // For SLEEP
    uint8_t sleep_ticks{0};

    // For FOR
    std::vector<Instruction> body;
    uint32_t repeats{0};

    // For READ
    std::string read_target_var;
    std::string read_addr_str;

    //FOR WRITE
    std::string write_addr_str;
    uint16_t write_value{0};

};

// =======================
// MCO2: Per-process memory
// =======================
struct PageEntry {
    int frame_id{-1};               // which physical frame holds this page (-1 = not in RAM)
    bool present{false};            // true if page currently in RAM
    bool in_backing_store{false};   // true if page data exists in backing store
    long backing_store_pos{-1};     // optional: byte/line offset in backing store file
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
    std::unordered_map<std::string, uint16_t> mem; // logical variables
    std::unordered_map<long, uint16_t> ram16; // address -> value

    std::vector<std::string> log; // For PRINT instruction

    // ============================
    // MCO2: per-process memory info
    // ============================
    long mem_bytes{0};                 // total virtual memory size for this process (bytes)
    int num_pages{0};                  // number of pages = ceil(mem_bytes / mem_per_frame)
    std::vector<PageEntry> page_table; // one entry per virtual page

    // --- REQUIREMENT 7 ADDITIONS: Crash Tracking ---
    bool crashed{false};               // Did the process crash?
    std::string crash_error_msg;       // The error message
    long crash_addr{0};                // The address that caused the crash
    std::string crash_time_str;        // Human readable time of crash
    // -----------------------------------------------

    // Stack for FOR loops
    struct LoopFrame {
        size_t for_instr_pc; // The index (in p.program) of the FOR_ instruction
        size_t body_pc;      // The current index within the FOR_ body
        uint32_t repeats_left;
    };
    std::vector<LoopFrame> loop_stack;
};

// All physical frames
std::vector<Frame> g_frames;
std::mutex g_frames_mtx;

// Physical frame contents: each frame stores `mem_per_frame` bytes
std::vector<std::vector<uint8_t>> g_frame_data;

std::queue<int> g_frame_fifo;   // frame ids used in allocation order (for FIFO replacement)

// Paging & CPU statistics (for vmstat / process-smi)
std::atomic<long> g_pages_paged_in{0};
std::atomic<long> g_pages_paged_out{0};
std::atomic<long> g_idle_cpu_ticks{0};
std::atomic<long> g_active_cpu_ticks{0};

std::atomic<int> g_cpu_cycles{0};
std::queue<int> g_ready_queue;
std::mutex g_ready_queue_mtx;
std::mutex g_rng_mtx;
std::atomic<int> g_attached_pid{-1};

std::vector<PseudoProcess> g_processes;
std::mutex g_processes_mtx;
int g_next_pid = 1;
std::atomic<bool> scheduler_generating{false};
std::thread scheduler;

// =======================
// MCO2: Backing store
// =======================
const std::string BACKING_STORE_FILE = "csopesy-backing-store.txt";
std::mutex g_backing_store_mtx;

// Reset / initialize backing store file (truncate + header)
void backing_store_reset() {
    std::lock_guard<std::mutex> lk(g_backing_store_mtx);
    std::ofstream ofs(BACKING_STORE_FILE, std::ios::trunc);
    if (ofs) {
        ofs << "# CSOPESY backing store\n";
        ofs << "# Each page entry will be written by the paging system.\n";
    }
}

// Append a log entry whenever a page is evicted to backing store.
void backing_store_write_page_entry(int pid, int page_index) {
    std::lock_guard<std::mutex> lk(g_backing_store_mtx);
    std::ofstream ofs(BACKING_STORE_FILE, std::ios::app);
    if (ofs) {
        ofs << "EVICT pid=" << pid << " page=" << page_index << "\n";
    }
}

// MCO2: Demand paging

// hex address to a long int
static long parse_hex_address(const std::string& s) {
    try {
        return std::stol(s, nullptr, 16);
    } catch (...) {
        return -1;
    }
}


// Convert a byte address (within a process) to a virtual page index.
int address_to_page_index(long addr) {
    long page_size = g_config.mem_per_frame;
    if (page_size <= 0) page_size = 1;
    if (addr < 0) return -1;
    return static_cast<int>(addr / page_size);
}

// Offset within the page (0 .. page_size-1)
long page_offset_within_page(long addr) {
    long page_size = g_config.mem_per_frame;
    if (page_size <= 0) page_size = 1;
    if (addr < 0) return -1;
    return addr % page_size;
}

// Helper: must be called with g_frames_mtx already locked.
static int find_free_frame_locked() {
    for (size_t i = 0; i < g_frames.size(); ++i) {
        if (!g_frames[i].used) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Helper: must be called with g_frames_mtx already locked.
static int select_victim_frame_fifo_locked() {
    while (!g_frame_fifo.empty()) {
        int fid = g_frame_fifo.front();
        g_frame_fifo.pop();
        if (fid >= 0 && fid < static_cast<int>(g_frames.size())) {
            if (g_frames[fid].used) {
                // Oldest still-in-use frame; choose it as victim.
                return fid;
            }
        }
    }
    // Fallback: scan for any used frame if queue is empty or stale.
    for (size_t i = 0; i < g_frames.size(); ++i) {
        if (g_frames[i].used) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Basic page-fault handler: load page into a free frame.
// NOTE: For now, this does NOT do replacement; if no frame is free, it fails.
bool handle_page_fault(PseudoProcess& proc, int page_index) {
    if (page_index < 0 || page_index >= proc.num_pages) {
        return false;
    }

    int frame_id = -1;
    int victim_pid = -1;
    int victim_page_index = -1;

    {
        std::lock_guard<std::mutex> lk(g_frames_mtx);

        // 1) Try to find a free frame first.
        frame_id = find_free_frame_locked();

        // 2) If none, select a victim frame via FIFO and evict.
        if (frame_id == -1) {
            int victim_frame = select_victim_frame_fifo_locked();
            if (victim_frame == -1) {
                // No frames at all (should not happen if memory is configured correctly).
                return false;
            }

            Frame& vf = g_frames[victim_frame];
            victim_pid = vf.owner_pid;
            victim_page_index = vf.page_index;

            // Log eviction to backing store (logical only, not real bytes yet)
            backing_store_write_page_entry(victim_pid, victim_page_index);
            g_pages_paged_out.fetch_add(1);

            // Mark the victim frame as available for new page
            frame_id = victim_frame;
            // The PageEntry of the victim process will be updated after we leave this mutex.

            // Note: we keep vf.used = true (frame stays allocated) and just overwrite
            //       owner_pid/page_index below when assigning to the new process.
        }

        // Assign this frame to the faulting process + page
        Frame& fr = g_frames[frame_id];
        fr.used = true;
        fr.owner_pid = proc.pid;
        fr.page_index = page_index;

        // For FIFO: every time a frame is used for a page, enqueue it.
        g_frame_fifo.push(frame_id);
    }

    // 3) If we evicted someone, update their page_table (no extra locks – caller
    //    already holds g_processes_mtx in cpu_core_function).
    if (victim_pid != -1) {
        for (auto& other : g_processes) {
            if (other.pid == victim_pid) {
                if (victim_page_index >= 0 &&
                    victim_page_index < static_cast<int>(other.page_table.size())) {

                    PageEntry& vpe = other.page_table[victim_page_index];
                    vpe.present = false;
                    vpe.frame_id = -1;
                    vpe.in_backing_store = true;
                    // backing_store_pos can remain -1 since we don't store real bytes.
                }
                break;
            }
        }
    }

    // 4) Finally, update the faulting process's page table.
    PageEntry& pe = proc.page_table[page_index];
    pe.frame_id = frame_id;
    pe.present = true;
    // This page is now in RAM. You can choose to keep in_backing_store as false or true
    // depending on whether you want write-back or write-through behavior; for now:
    pe.in_backing_store = false;

    g_pages_paged_in.fetch_add(1);
    return true;
}

// Ensure a page is resident; used later by READ/WRITE.
bool ensure_page_loaded(PseudoProcess& proc, int page_index) {
    if (page_index < 0 || page_index >= proc.num_pages) {
        return false;
    }
    PageEntry& pe = proc.page_table[page_index];
    if (pe.present) return true;
    return handle_page_fault(proc, page_index);
}

// Result of translating and validating a virtual memory address.
struct AddressResult {
    bool ok{false};          // true if address is valid and page loaded
    int page_index{-1};      // virtual page index
    long offset{-1};         // offset within page (0 .. page_size-1)
    std::string error_msg;   // reason for failure
};

// Validate virtual address and ensure the page is loaded.
// This is the main helper your READ/WRITE instructions will use.
AddressResult check_address_and_load(PseudoProcess& proc, long addr) {
    AddressResult res;

    // 1) Validate address range
    if (addr < 0 || addr >= proc.mem_bytes) {
        res.ok = false;
        res.error_msg = "Invalid memory address: out of process memory range.";
        return res;
    }

    // 2) Compute virtual page index & offset
    long page_size = g_config.mem_per_frame;
    if (page_size <= 0) page_size = 1;

    int page_index = static_cast<int>(addr / page_size);
    long offset    = addr % page_size;

    // 3) Ensure the page is loaded (may cause page fault + replacement)
    if (!ensure_page_loaded(proc, page_index)) {
        res.ok = false;
        res.error_msg = "Page fault could not be resolved (no free frames or replacement failed).";
        return res;
    }

    // 4) Success
    res.ok = true;
    res.page_index = page_index;
    res.offset = offset;
    return res;
}

// For instructions that require checking address only
bool validate_address_only(PseudoProcess& proc, long addr, std::string& err) {
    auto res = check_address_and_load(proc, addr);
    if (!res.ok) {
        err = res.error_msg;
        return false;
    }
    return true;
}

// For retrieving frame ID + final physical location
bool translate_address(PseudoProcess& proc, long addr, int& frame_id, long& offset, std::string& err) {
    auto res = check_address_and_load(proc, addr);
    if (!res.ok) {
        err = res.error_msg;
        return false;
    }

    // Extracting the frame from the page table
    PageEntry& pe = proc.page_table[res.page_index];
    frame_id = pe.frame_id;
    offset = res.offset;
    return true;
}

// Read a uint16_t value at the given virtual byte address (little-endian).
// Handles page loads and page-crossing reads by translating each byte separately.
bool read_u16_at(PseudoProcess& proc, long addr, uint16_t& out_val, std::string& err) {
    int f0 = -1, f1 = -1;
    long o0 = -1, o1 = -1;
    if (!translate_address(proc, addr, f0, o0, err)) return false;
    if (!translate_address(proc, addr + 1, f1, o1, err)) return false;

    std::lock_guard<std::mutex> lk(g_frames_mtx);
    if (f0 < 0 || f0 >= static_cast<int>(g_frame_data.size()) ||
        f1 < 0 || f1 >= static_cast<int>(g_frame_data.size())) {
        err = "Invalid frame id while reading memory.";
        return false;
    }

    long frame_sz = g_config.mem_per_frame;
    if (o0 < 0 || o0 >= frame_sz || o1 < 0 || o1 >= frame_sz) {
        err = "Invalid offset while reading memory.";
        return false;
    }

    uint8_t b0 = g_frame_data[f0][o0];
    uint8_t b1 = g_frame_data[f1][o1];
    out_val = static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
    return true;
}

// Write a uint16_t value at the given virtual byte address (little-endian).
bool write_u16_at(PseudoProcess& proc, long addr, uint16_t val, std::string& err) {
    int f0 = -1, f1 = -1;
    long o0 = -1, o1 = -1;
    if (!translate_address(proc, addr, f0, o0, err)) return false;
    if (!translate_address(proc, addr + 1, f1, o1, err)) return false;

    std::lock_guard<std::mutex> lk(g_frames_mtx);
    if (f0 < 0 || f0 >= static_cast<int>(g_frame_data.size()) ||
        f1 < 0 || f1 >= static_cast<int>(g_frame_data.size())) {
        err = "Invalid frame id while writing memory.";
        return false;
    }

    long frame_sz = g_config.mem_per_frame;
    if (o0 < 0 || o0 >= frame_sz || o1 < 0 || o1 >= frame_sz) {
        err = "Invalid offset while writing memory.";
        return false;
    }

    uint8_t b0 = static_cast<uint8_t>(val & 0xFF);
    uint8_t b1 = static_cast<uint8_t>((val >> 8) & 0xFF);
    g_frame_data[f0][o0] = b0;
    g_frame_data[f1][o1] = b1;
    return true;
}

// shared state
bool is_initialized = false;
size_t display_width = 100;         //TO-DO : do we need this
std::queue<char> key_buffer;    //TO-DO : do we need this
std::mutex key_buffer_mutex;    //TO-DO : do we need this
std::atomic<bool> is_running{true};

//HELPER FUNCTION
// Helper: Get current time as string HH:MM:SS
std::string get_current_time_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
    #if defined(_WIN32) || defined(_WIN64)
        localtime_s(&bt, &now_time); 
    #else
        localtime_r(&now_time, &bt);
    #endif
    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S");
    return oss.str();
}

// HELPER FUNCTION (UPDATED)
// Handles "quoted strings" and escaped quotes \" correctly
vector<string> tokenize_input(const string& input) {
    vector<string> tokens;
    string current_token;
    bool in_quotes = false;

    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];

        // Handle escaped quote \" -> treat as literal "
        if (c == '\\' && i + 1 < input.length() && input[i + 1] == '"') {
            current_token += '"'; // Add " as a literal character
            i++; // Skip the next char (the quote)
        }
        else if (c == '"') {
            in_quotes = !in_quotes; // Toggle quote state
        } 
        else if (c == ' ' && !in_quotes) {
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
        } 
        else {
            current_token += c;
        }
    }
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }
    return tokens;
}

//HELPER FUNCTION
void clear_screen() {
    // This is a common cross-platform way.
    // \033[2J clears the screen, \033[H moves cursor to top-left.
    cout << "\033[2J\033[H";
}

// this function checks if size is a power of 2 and between 2^6 (64) and 2^16 (65536)
bool is_valid_mem_size(long size) {
    if (size < 64 || size > 65536) {
        return false;
    }

    return (size > 0) && ((size & (size - 1)) == 0);
}

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

// [Requirement 6] Parse a single instruction string line (e.g. "DECLARE a 5")
Instruction parse_line(const std::string& line) {
    Instruction instr;
    std::stringstream ss(line);
    std::string type_str;
    ss >> type_str;

    if (type_str == "DECLARE") {
        instr.type = InstrType::DECLARE;
        ss >> instr.var >> instr.value;
    }
    else if (type_str == "PRINT") {
        instr.type = InstrType::PRINT;
        size_t start = line.find("PRINT") + 5;
        if (start < line.size()) {
            std::string content = line.substr(start);
            size_t first = content.find_first_not_of(" (");
            size_t last = content.find_last_not_of(" )");
            if(first != std::string::npos) 
                instr.msg = content.substr(first, (last - first + 1));
        }
    }
    else if (type_str == "ADD") {
        instr.type = InstrType::ADD;
        std::string v2, v3;
        ss >> instr.var1 >> v2 >> v3;
        if (isdigit(v2[0])) { instr.var2_is_literal = true; instr.lit2 = stoi(v2); }
        else { instr.var2 = v2; }
        if (isdigit(v3[0])) { instr.var3_is_literal = true; instr.lit3 = stoi(v3); }
        else { instr.var3 = v3; }
    }
    else if (type_str == "SUBTRACT") {
        instr.type = InstrType::SUBTRACT;
        std::string v2, v3;
        ss >> instr.var1 >> v2 >> v3;
        if (isdigit(v2[0])) { instr.var2_is_literal = true; instr.lit2 = stoi(v2); }
        else { instr.var2 = v2; }
        if (isdigit(v3[0])) { instr.var3_is_literal = true; instr.lit3 = stoi(v3); }
        else { instr.var3 = v3; }
    }
    else if (type_str == "WRITE") {
        instr.type = InstrType::WRITE;
        std::string addr_str, val_str;
        ss >> addr_str >> val_str;
        instr.lit2 = std::stoul(addr_str, nullptr, 16); 
        if (isdigit(val_str[0])) { 
            instr.var3_is_literal = true; 
            instr.lit3 = stoi(val_str); 
        } else {
            instr.var1 = val_str; 
        }
    }
    else if (type_str == "READ") {
        instr.type = InstrType::READ;
        std::string addr_str;
        ss >> instr.var >> addr_str;
        instr.lit2 = std::stoul(addr_str, nullptr, 16); 
    }
    
    return instr;
}

// [Requirement 6] Parse the full semi-colon separated string
std::vector<Instruction> parse_custom_program(const std::string& script) {
    std::vector<Instruction> prog;
    std::stringstream ss(script);
    std::string segment;

    while (std::getline(ss, segment, ';')) {
        size_t first = segment.find_first_not_of(" ");
        if (first == std::string::npos) continue; 
        std::string clean_line = segment.substr(first);
        prog.push_back(parse_line(clean_line));
    }
    return prog;
}

static std::vector<Instruction> make_default_program(const std::string& pname) {
    std::vector<Instruction> prog;

    Instruction d; d.type = InstrType::DECLARE; d.var = "x"; d.value = 0; prog.push_back(d);

    // Add a small WRITE then READ sequence so generated processes exercise memory ops
    Instruction decl_mem; decl_mem.type = InstrType::DECLARE; decl_mem.var = "memv"; decl_mem.value = 0; prog.push_back(decl_mem);
    Instruction wr1; wr1.type = InstrType::WRITE; wr1.write_addr_str = "0x0"; wr1.write_value = 42; prog.push_back(wr1);
    Instruction rd1; rd1.type = InstrType::READ; rd1.read_addr_str = "0x0"; rd1.read_target_var = "memv"; prog.push_back(rd1);

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

// --- SCHEDULER RANDOMIZATION HELPERS ---

// Helper for random integers
int rand_int(int min, int max) {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

// Generate a random instruction
Instruction make_random_instruction(long max_mem) {
    Instruction instr;
    // 0=PRINT, 1=DECLARE, 2=ADD, 3=SUB, 4=SLEEP, 5=WRITE, 6=READ
    // We weight them slightly to make sure we get enough memory ops
    int type_idx = rand_int(0, 8); 
    
    // Map random index to type
    if (type_idx == 0) instr.type = InstrType::PRINT;
    else if (type_idx == 1) instr.type = InstrType::DECLARE;
    else if (type_idx == 2) instr.type = InstrType::ADD;
    else if (type_idx == 3) instr.type = InstrType::SUBTRACT;
    else if (type_idx == 4) instr.type = InstrType::SLEEP;
    else if (type_idx == 5 || type_idx == 7) instr.type = InstrType::WRITE; // Higher chance
    else if (type_idx == 6 || type_idx == 8) instr.type = InstrType::READ;  // Higher chance

    switch (instr.type) {
        case InstrType::PRINT:
            instr.msg = "Auto-generated message";
            break;
        case InstrType::DECLARE:
            instr.var = "v" + std::to_string(rand_int(1, 5)); // v1..v5
            instr.value = rand_int(1, 100);
            break;
        case InstrType::ADD:
        case InstrType::SUBTRACT:
            instr.var1 = "v" + std::to_string(rand_int(1, 5));
            instr.var2 = "v" + std::to_string(rand_int(1, 5));
            instr.var3_is_literal = true; 
            instr.lit3 = rand_int(1, 10);
            break;
        case InstrType::SLEEP:
            instr.sleep_ticks = rand_int(1, 5);
            break;
        case InstrType::WRITE: {
            // Write to a random address within the process memory limit
            // We align to 4 bytes just to be clean, though not strictly required
            long rand_addr = rand_int(0, (max_mem > 0 ? max_mem - 1 : 0));
            instr.lit2 = rand_addr; 
            instr.var3_is_literal = true;
            instr.lit3 = rand_int(0, 255); // Write random value
            break;
        }
        case InstrType::READ: {
            instr.var = "v" + std::to_string(rand_int(1, 5));
            long rand_addr = rand_int(0, (max_mem > 0 ? max_mem - 1 : 0));
            instr.lit2 = rand_addr;
            break;
        }
        case InstrType::FOR_: break; // We skip FOR generation for simplicity in this randomizer
    }
    return instr;
}

std::vector<Instruction> make_random_program(long mem_size) {
    std::vector<Instruction> prog;
    long num_ins = rand_int(g_config.min_ins, g_config.max_ins);
    
    // Always start with a Declaration so we have a variable to use
    Instruction d; d.type = InstrType::DECLARE; d.var = "v1"; d.value = 0;
    prog.push_back(d);

    for (int i = 0; i < num_ins; ++i) {
        prog.push_back(make_random_instruction(mem_size));
    }
    return prog;
}

// Scheduler Start
void scheduler_start() {
    scheduler_generating = true;
    long freq = g_config.batch_process_freq;
    if (freq <= 0) freq = 1;

    long long last_tick = g_cpu_cycles.load();

    while (scheduler_generating && is_running) {
        long long cur = g_cpu_cycles.load();
        if (cur - last_tick >= freq) {

            int running_count = 0;
            int ready_count = 0;

            // count running and ready processes
            {
                std::lock_guard<std::mutex> lk1(g_processes_mtx);
                for (auto& p : g_processes) {
                    if (p.running && !p.finished) running_count++;
                }
            }
            {
                std::lock_guard<std::mutex> lk2(g_ready_queue_mtx);
                ready_count = (int)g_ready_queue.size();
            }

            int active_total = running_count + ready_count;

            // Only generate if fewer than num_cpu active processes
            // Only generate if fewer than num_cpu active processes
            if (active_total < g_config.num_cpu) {
                int to_generate = g_config.num_cpu - active_total;
                for (int i = 0; i < to_generate; ++i) {
                    PseudoProcess proc;
                    {
                        std::lock_guard<std::mutex> lk(g_processes_mtx);
                        proc.pid = g_next_pid++;
                    }

                    std::ostringstream pname_ss;
                    pname_ss << "p";
                    if (proc.pid < 10) pname_ss << "0";
                    pname_ss << proc.pid;
                    proc.name = pname_ss.str();
                    proc.start_time = std::chrono::steady_clock::now();
                    proc.running = false;

                    // [CHANGE] Randomize memory size FIRST
                    // Calculate power of 2 size between min and max
                    // Simple approach: pick a size, then verify validity or just pick standard sizes
                    // For simplicity, we pick either min, max, or something in between
                    long mem_opts[] = {64, 256, 1024, 4096, 16384};
                    int m_idx = rand_int(0, 4);
                    long chosen_mem = mem_opts[m_idx];
                    
                    // Clamp to config limits
                    if (chosen_mem < g_config.min_mem_per_proc) chosen_mem = g_config.min_mem_per_proc;
                    if (chosen_mem > g_config.max_mem_per_proc) chosen_mem = g_config.max_mem_per_proc;

                    proc.mem_bytes = chosen_mem;

                    // [CHANGE] Use Random Program Generator
                    // We pass chosen_mem so it generates addresses within bounds
                    proc.program = make_random_program(proc.mem_bytes);
                    
                    // Setup Pages
                    long page_size = g_config.mem_per_frame;
                    if (page_size <= 0) page_size = 1;
                    proc.num_pages = static_cast<int>((proc.mem_bytes + page_size - 1) / page_size);
                    proc.page_table.assign(proc.num_pages, PageEntry{});

                    {
                        std::lock_guard<std::mutex> lk(g_processes_mtx);
                        g_processes.push_back(std::move(proc));
                    }

                    {
                        std::lock_guard<std::mutex> lk(g_ready_queue_mtx);
                        g_ready_queue.push(g_next_pid - 1);
                    }
                    
                    std::cout << "[scheduler] generated " << proc.name 
                              << " (" << proc.mem_bytes << " bytes)\n";
                }
            }

            last_tick = cur;
        }

        // give CPU threads time to pick up work
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
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

    int cores_used = 0;
    for (const auto& p : g_processes) {
        // 'running' == true means it's on a core OR sleeping
        if (p.running) { 
            cores_used++;
        }
    }
    int cores_available = g_config.num_cpu - cores_used;
    if (cores_available < 0) cores_available = 0; // Safety check
    
    double cpu_utilization = 0.0;
    if (g_config.num_cpu > 0) {
        cpu_utilization = (static_cast<double>(cores_used) / g_config.num_cpu) * 100.0;
    }

    oss << "CPU utilization: " << (int)cpu_utilization << "%\n";
    oss << "Cores used: " << cores_used << "\n";
    oss << "Cores available: " << cores_available << "\n\n";
    
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

        case InstrType::DECLARE: {
            // Only declare if variable is new and symbol table has space
            if (p.mem.find(instr.var) == p.mem.end()) {
                if (p.mem.size() >= 32) {
                    p.log.push_back("Warning: Symbol table full, ignoring declaration.");
                    break;
                }
            }

            // Symbol table memory is simulated as Page 0 access
            std::string err;
            if (!validate_address_only(p, 0, err)) {
                p.crashed = true;
                p.crash_error_msg = "Page fault on symbol table access: " + err;
                break;
            }

            // Declare or update the variable
            p.mem[instr.var] = instr.value;
            break;
        }

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
        case InstrType::READ: {
            // READ (var, memory_address)
            long addr = parse_hex_address(instr.read_addr_str);
            if (addr < 0) {
                std::cout << "Process " << p.pid << ": invalid read address '" << instr.read_addr_str << "'. Terminating process.\n";
                p.finished = true;
                return ExecStatus::FINISHED;
            }

            uint16_t val = 0;
            std::string err;
            if (!read_u16_at(p, addr, val, err)) {
                std::cout << "Process " << p.pid << ": memory access violation at address " << instr.read_addr_str << ": " << err << "\n";
                p.finished = true;
                return ExecStatus::FINISHED;
            }

            // Auto-declare and store
            p.mem[instr.read_target_var] = val;
            break;
        }
        case InstrType::WRITE: {
            // WRITE (memory_address, value)
            long addr = parse_hex_address(instr.write_addr_str);
            if (addr < 0) {
                std::cout << "Process " << p.pid << ": invalid write address '" << instr.write_addr_str << "'. Terminating process.\n";
                p.finished = true;
                return ExecStatus::FINISHED;
            }

            std::string err;
            if (!write_u16_at(p, addr, instr.write_value, err)) {
                std::cout << "Process " << p.pid << ": memory access violation at address " << instr.write_addr_str << ": " << err << "\n";
                p.finished = true;
                return ExecStatus::FINISHED;
            }
            break;
        }
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
            // [CHANGE 7] PASTE VIOLATION CHECK HERE
            // Check for memory violations (Simulated for Requirement 7)
            if (instr_to_exec->type == InstrType::WRITE || instr_to_exec->type == InstrType::READ) {
                long target_addr = instr_to_exec->lit2; 
                std::string err;
                
                // Use your existing validator to check if address is valid
                if (!validate_address_only(*p, target_addr, err)) {
                    // CRASH THE PROCESS
                    p->crashed = true;
                    p->crash_time_str = get_current_time_str();
                    p->crash_addr = target_addr;
                    p->finished = true; 
                    p->running = false;
                    process_finished = true; // Stop execution immediately
                    
                    // Optional: Add to log for process-smi
                    p->log.push_back("Start time: " + p->crash_time_str);
                    p->log.push_back("Crash: Memory violation at 0x" + std::to_string(target_addr));
                    break; 
                }
            }
            // [END CHANGE 7]

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

    // check if in attached mode
    int attached_pid = g_attached_pid.load();

    // attached mode
    if (attached_pid != -1) {
        if (cmd == "exit") {
            g_attached_pid = -1; 
            clear_screen();
            cout << "Returned to main console.\n";
        } 
        else if (cmd == "process-smi") {
            std::lock_guard<std::mutex> lk(g_processes_mtx);
            PseudoProcess* p_ptr = nullptr;
            for(auto& p : g_processes) {
                if(p.pid == attached_pid) {
                    p_ptr = &p;
                    break;
                }
            }

            if (p_ptr == nullptr) {
                cout << "Error: Process " << attached_pid << " not found.\n";
                g_attached_pid = -1; 
                return;
            }

            cout << "Process name: " << p_ptr->name << "\n";
            cout << "ID: " << p_ptr->pid << "\n";
            
            cout << "Logs:\n";
            if (p_ptr->log.empty()) {
                cout << "  (No log output)\n";
            } else {
                for(const auto& log_msg : p_ptr->log) {
                    cout << "  " << log_msg << "\n";
                }
            }
            
            cout << "Current instruction line: " << p_ptr->pc << "\n";
            cout << "Total lines of code: " << p_ptr->program.size() << "\n";

            if (p_ptr->finished) {
                cout << "Finished!\n";
            }
        }
        else {
            cout << "Unknown command. Valid commands in process screen are:\n"
                 << "  'process-smi' - print process status\n"
                 << "  'exit'          - return to main console\n";
        }
        return;
    }

    // main console mode
    
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
                    if (!value_str.empty() && value_str.front() == '"') value_str.erase(0, 1);
                    if (!value_str.empty() && value_str.back() == '"') value_str.pop_back();
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
                // ===== MCO2 memory config keys =====
                else if (key == "max-overall-mem") {
                    g_config.max_overall_mem = std::stol(value_str);
                } else if (key == "mem-per-frame") {
                    g_config.mem_per_frame = std::stol(value_str);
                } else if (key == "min-mem-per-proc") {
                    g_config.min_mem_per_proc = std::stol(value_str);
                } else if (key == "max-mem-per-proc") {
                    g_config.max_mem_per_proc = std::stol(value_str);
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

        // MCO2 memory configuration
        cout << "  - max-overall-mem: " << g_config.max_overall_mem << " bytes\n";
        cout << "  - mem-per-frame: "   << g_config.mem_per_frame   << " bytes\n";
        cout << "  - min-mem-per-proc: " << g_config.min_mem_per_proc << " bytes\n";
        cout << "  - max-mem-per-proc: " << g_config.max_mem_per_proc << " bytes\n";
        
        // ==========================
        // MCO2: initialize frames
        // ==========================
        {
            long num_frames = 0;
            if (g_config.mem_per_frame > 0 && g_config.max_overall_mem > 0) {
                num_frames = g_config.max_overall_mem / g_config.mem_per_frame;
            }

            {
                std::lock_guard<std::mutex> lk(g_frames_mtx);
                g_frames.clear();
                if (num_frames > 0) {
                    g_frames.reserve(num_frames);
                    g_frame_data.clear();
                    g_frame_data.resize(static_cast<size_t>(num_frames));
                    for (long i = 0; i < num_frames; ++i) {
                        Frame fr;
                        fr.frame_id = static_cast<int>(i);
                        // used=false, owner_pid=-1, page_index=-1 by default
                        g_frames.push_back(fr);
                        // initialize frame bytes to zero
                        g_frame_data[static_cast<size_t>(i)].assign(static_cast<size_t>(g_config.mem_per_frame), 0);
                    }
                }
            }

            cout << "Physical memory configured with " << num_frames
                 << " frame(s) of " << g_config.mem_per_frame << " bytes each "
                 << "(" << g_config.max_overall_mem << " bytes total).\n";
        }
        
        // MCO2: reset backing store file
        backing_store_reset();
        cout << "Backing store file \"" << BACKING_STORE_FILE << "\" reset.\n";
        
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
        cout << "\"screen -s <program name> <memory size>\" - creates a new process with given memory size and attaches to it\n";
        cout << "\"screen -r <program name>\" - re-attaches to a running process\n";
        cout << "\"screen -ls\" - lists all running processes\n";
        cout << "\"scheduler-start\" - start the scheduler which continuously generates a batch of dummy processes for the CPU scheduler\n";
        cout << "\"scheduler-stop\" - stop the scheduler/generating dummy processes \n";
        cout << "\"report-util\" - generate of CPU utilization report\n";
    }
    else if (cmd == "screen") {
        if (tokens.size() == 1) {
        cout << "Usage:\n"
             << "  screen -s <process name> <memory size>  Create a new process with memory and attach\n"
             << "  screen -r <process name>   Re-attach to a process\n"
             << "  screen -ls                 List running processes\n";
        return;
    	}

    	if (tokens[1] == "-ls") {
        	report_utilization();
        	return;
    	}

    	if (tokens[1] == "-s") {
        	if (tokens.size() < 4) { // Now requires 4 tokens: "screen", "-s", <name>, <size>
            	cout << "Error: Usage: screen -s <process_name> <memory_size_bytes>.\n";
            	return;
        	}
        
            std::string mem_size_str = tokens.back();
            long mem_size = 0;
            try {
                mem_size = std::stol(mem_size_str);
            } catch (const std::exception& e) {
                cout << "Error: Invalid memory allocation (must be a number).\n";
                return;
            }

            // MCO2: New validation step
            if (!is_valid_mem_size(mem_size)) {
                cout << "Error: Invalid memory allocation (must be between 64 and 65536 bytes, and a power of 2).\n";
                return;
            }

        	std::ostringstream oss;
        	for (size_t i = 2; i < tokens.size() - 1; ++i) { // Name is now all tokens between -s and the last token (size)
            	if (i > 2) oss << ' ';
            	oss << tokens[i];
        	}
        	std::string pname = oss.str();
            
            // Check if name is empty (e.g., if only size was passed as name)
            if (pname.empty()) {
                cout << "Error: missing <process name>.\n";
                return;
            }

        	PseudoProcess proc;
        	proc.pid = g_next_pid++;
        	proc.name = pname;
        	proc.start_time = std::chrono::steady_clock::now();
        	proc.running = false;
        	proc.program = make_default_program(pname);
        	
        	// MCO2: Set memory configuration based on user input
    		proc.mem_bytes = mem_size;
    		long page_size = g_config.mem_per_frame;
    		if (page_size <= 0) page_size = 1;
    		// Calculate number of pages (mem_bytes is guaranteed power of 2, so ceiling is just division)
    		proc.num_pages = static_cast<int>(proc.mem_bytes / page_size); 
    		
    		// Check against minimum memory (64 bytes)
    		if (proc.mem_bytes < g_config.min_mem_per_proc) {
    		    // This shouldn't be reached if is_valid_mem_size is correct, but safe check.
    		    cout << "Error: Process must require memory of at least " << g_config.min_mem_per_proc << " bytes.\n";
    		    return;
    		}
    		
    		// Check if process mem is a multiple of frame size
    		if (proc.mem_bytes % page_size != 0) {
    		    // This is a safety check; if mem_per_frame is a power of 2 and mem_size is a power of 2, this is likely true.
    		    cout << "Error: Process memory size (" << mem_size << " bytes) must be a multiple of frame size (" << page_size << " bytes).\n";
    		    return;
    		}
    		
    		proc.page_table.assign(proc.num_pages, PageEntry{});
            int new_pid = proc.pid; // Store PID

        	{
            	std::lock_guard<std::mutex> lk(g_processes_mtx);
            	g_processes.push_back(std::move(proc));
        	}

            {
                std::lock_guard<std::mutex> lk(g_ready_queue_mtx);
                g_ready_queue.push(new_pid);
            }
        	

            cout << "Started process \"" << pname << "\" with PID " << new_pid 
                 << " and allocated " << mem_size << " bytes.\n";
            cout << "Attaching to process...\n";
            g_attached_pid = new_pid; 
            clear_screen();
        	return;
    	}

// [Requirement 6] screen -c implementation
        if (tokens[1] == "-c") {
            if (tokens.size() < 5) { // Needs: screen -c <name> <size> <instruction>
                cout << "Error: Usage: screen -c <name> <mem_size> \"<instructions>\"\n";
                return;
            }
            
            string pname = tokens[2];
            long mem_size = 0;
             try {
                mem_size = std::stol(tokens[3]);
            } catch (...) { cout << "Invalid size.\n"; return; }

            string instruction_script = tokens[4]; 

            PseudoProcess proc;
            proc.pid = g_next_pid++;
            proc.name = pname;
            proc.start_time = std::chrono::steady_clock::now();
            proc.running = false;
            
            proc.mem_bytes = mem_size;
            long page_size = g_config.mem_per_frame;
            if (page_size <= 0) page_size = 1;
            proc.num_pages = static_cast<int>((proc.mem_bytes + page_size - 1) / page_size);
            proc.page_table.assign(proc.num_pages, PageEntry{});

            // Load Custom Instructions
            proc.program = parse_custom_program(instruction_script);

            int new_pid = proc.pid;
            {
                std::lock_guard<std::mutex> lk(g_processes_mtx);
                g_processes.push_back(std::move(proc));
            }
            {
                std::lock_guard<std::mutex> lk(g_ready_queue_mtx);
                g_ready_queue.push(new_pid);
            }
            cout << "Custom process \"" << pname << "\" created with PID " << new_pid << ".\n";

            cout << "Attaching to process...\n";
            g_attached_pid = new_pid; 
            clear_screen();
            
            return;
        }

        // [Requirement 7] screen -r Update for Violation Errors
        if (tokens[1] == "-r") {
             if (tokens.size() < 3) { cout << "Error: missing <process name>.\n"; return; }
             
             std::string pname;
             // Combine tokens in case name has spaces (though your tokenizer handles quotes now)
             std::ostringstream oss;
             for (size_t i = 2; i < tokens.size(); ++i) {
                if (i > 2) oss << ' ';
                oss << tokens[i];
             }
             pname = oss.str();

            int pid_to_attach = -1;
            bool is_crashed = false;
            long crash_addr = 0;
            std::string crash_time;

            {
                std::lock_guard<std::mutex> lk(g_processes_mtx);
                for (auto& p : g_processes) {
                    if (p.name == pname) {
                        if (p.crashed) {
                            is_crashed = true;
                            crash_addr = p.crash_addr;
                            crash_time = p.crash_time_str;
                            break;
                        }
                        if (!p.finished) {
                            pid_to_attach = p.pid;
                        }
                        break;
                    }
                }
            }

            if (is_crashed) {
                cout << "Process " << pname << " shut down due to memory access violation error that occurred at " 
                     << crash_time << ".\n0x" << std::hex << crash_addr << std::dec << " invalid.\n";
            } 
            else if (pid_to_attach != -1) {
                g_attached_pid = pid_to_attach;
                clear_screen();
                cout << "Re-attached to process " << pname << ".\n";
            } else {
                cout << "Error: Process <" << pname << "> not found or has finished.\n";
            }
            return;
        }

    	cout << "Unknown 'screen' option. Try: 'help'\n";
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
    else if (cmd == "process-smi") {

    // ----- compute memory usage from frames -----
    long total_mem   = g_config.max_overall_mem;
    long frame_size  = g_config.mem_per_frame;
    long used_frames = 0;

    {
        std::lock_guard<std::mutex> lk(g_frames_mtx);
        for (const auto &fr : g_frames) {
            if (fr.used) {
                ++used_frames;
            }
        }
    }

    long used_mem = used_frames * frame_size;
    long free_mem = total_mem - used_mem;
    if (free_mem < 0) free_mem = 0;

    cout << "==================== PROCESS-SMI ====================\n";
    cout << "System Memory Summary\n";
    cout << "-----------------------------------------------------\n";
    cout << "Total Memory:        " << total_mem   << " bytes\n";
    cout << "Used Memory:         " << used_mem    << " bytes\n";
    cout << "Free Memory:         " << free_mem    << " bytes\n";
    cout << "\nRunning Processes:\n";

    {
        std::lock_guard<std::mutex> lk(g_processes_mtx);
        if (g_processes.empty()) {
            cout << "  (No running processes)\n";
        } else {
            for (auto &p : g_processes) {
            	int pages_in_ram = 0;
            	int pages_swapped = 0;
            	for (const auto &pe : p.page_table) {
                	if (pe.present) {
                    	pages_in_ram++;
                	} else if (pe.in_backing_store) {
                    	pages_swapped++;
                	}
            	}

            	cout << "PID " << p.pid
                 	<< " | Name: " << p.name
                 	<< " | Mem: " << p.mem_bytes << " bytes"
                 	<< " | Pages in RAM: " << pages_in_ram
                 	<< " | Pages swapped: " << pages_swapped << "\n";
        	}

        }
    }
    cout << "=====================================================\n";
	}
    else if (cmd == "vmstat") {

    // ----- compute memory usage from frames -----
    long total_mem   = g_config.max_overall_mem;
    long frame_size  = g_config.mem_per_frame;
    long used_frames = 0;

    {
        std::lock_guard<std::mutex> lk(g_frames_mtx);
        for (const auto &fr : g_frames) {
            if (fr.used) {
                ++used_frames;
            }
        }
    }

    long used_mem = used_frames * frame_size;
    long free_mem = total_mem - used_mem;
    if (free_mem < 0) free_mem = 0;

    long idle_ticks   = g_idle_cpu_ticks.load();
    long active_ticks = g_active_cpu_ticks.load();
    long total_ticks  = g_cpu_cycles.load(); // existing global system clock

    long paged_in  = g_pages_paged_in.load();
    long paged_out = g_pages_paged_out.load();

    cout << "========================= VMSTAT =========================\n";
    cout << "Memory Report\n";
    cout << "----------------------------------------------------------\n";
    cout << "Total Memory:        " << total_mem   << " bytes\n";
    cout << "Used Memory:         " << used_mem    << " bytes\n";
    cout << "Free Memory:         " << free_mem    << " bytes\n";
    cout << "Idle CPU ticks:      " << idle_ticks   << "\n";
    cout << "Active CPU ticks:    " << active_ticks << "\n";
    cout << "Total CPU ticks:     " << total_ticks  << "\n";
    cout << "Pages paged in:      " << paged_in     << "\n";
    cout << "Pages paged out:     " << paged_out    << "\n";
    cout << "==========================================================\n";
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
    string input;

    thread displayThread(display_handler_thread);
    thread keyboardThread(keyboard_handler_thread);
    thread schedulerThread(scheduler_thread);
	
    std::string current_prompt = "Command> ";
    cout << current_prompt;

    while(is_running){
        {
            std::lock_guard<std::mutex> lock(key_buffer_mutex);
            while (!key_buffer.empty()) {
                char ch = key_buffer.front();
                key_buffer.pop();
                
                if (ch == '\r') { // enter
                    cout << endl;
                    thread commandThread(command_interpreter_thread, input);
                    commandThread.join();
                    input.clear();
                    
                    // prompt
                    int attached_pid = g_attached_pid.load();
                    if (attached_pid != -1) {
                        // process screen
                        std::lock_guard<std::mutex> lk(g_processes_mtx);
                        std::string pname = "process";
                        for(auto& p : g_processes) {
                            if (p.pid == attached_pid) {
                                pname = p.name;
                                break;
                            }
                        }
                        current_prompt = pname + ":\\>";

                    } else {
                        // main menu
                        current_prompt = "Command> ";
                    }
                    cout << current_prompt;

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
    scheduler_stop(); // Clean up scheduler thread
    return 0;
}