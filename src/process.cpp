#include "process.h"

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;
    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }
    priority = details.priority;
    state = (start_time == 0) ? State::Ready : State::NotStarted;
    if (state == State::Ready)
    {
        launch_time = current_time;
    }
    is_interrupted = false;
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    termNumber = 0;
    readyQlastTime = current_time;
    runningQlastTime = current_time;
    for (i = 0; i < num_bursts; i+=2)
    {
        remain_time += burst_times[i];
    }
}

Process::~Process()
{
    delete[] burst_times;
}

uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0; 
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

uint32_t Process::getCurrentBurstTime(uint16_t burstIdx) const
{ 
    return burst_times[burstIdx];
}


void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == State::NotStarted && new_state == State::Ready)
    {
        launch_time = current_time;
    }
    state = new_state;
}

void Process::setCurrentBurst(uint16_t burstNum){
    current_burst = burstNum;
}

uint16_t Process::getCurrentBurst() const{
    return current_burst;
}

void Process::setCpuCore(int8_t core_num)
{
    core = core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{
    // use `current_time` to update turnaround time, wait time, burst times, 
    // cpu time, and remaining time

    turn_time = current_time - launch_time;

    if(state == State::Ready){
        wait_time += (current_time - readyQlastTime);
        readyQlastTime = current_time;
    }

    if(state == State::Running){
        cpu_time += (current_time - runningQlastTime);
        remain_time -= (current_time - runningQlastTime);

        runningQlastTime = current_time;
    }

    if(state == State::Terminated){ //this is only so Remain Time won't print out as -0.0
        remain_time = 0.0;
    }
}

void Process::setTermNumber(int termNum){
    termNumber = termNum;
}

int Process::getTermNumber(){
    return termNumber;
}

void Process::setReadyQlastTime(uint32_t current_time){
    readyQlastTime = current_time;
}
void Process::setRunningQlastTime(uint32_t current_time){
    runningQlastTime = current_time;
}

void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    burst_times[burst_idx] = new_time;
}

uint16_t Process::getNumberOfBursts() const {
    return num_bursts;
}


// Comparator methods: used in std::list sort() method
// No comparator needed for FCFS or RR (ready queue never sorted)

// SJF - comparator for sorting read queue based on shortest remaining CPU time
bool SjfComparator::operator ()(const Process *p1, const Process *p2)
{
    return ((*p1).getRemainingTime() < (*p2).getRemainingTime());
}

// PP - comparator for sorting read queue based on priority
bool PpComparator::operator ()(const Process *p1, const Process *p2)
{
    return ((*p1).getPriority() < (*p2).getPriority()); //lower num = higher priority
}