#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData
{
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;  
    std::list<Process *> ready_queue;
    bool all_terminated;
    int termNumber;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process *> &processes, std::mutex &mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process *> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;
    shared_data->termNumber = 0;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;

    while (!(shared_data->all_terminated))
    {
        // Clear output from previous iteration
        clearOutput(num_lines);

        // Do the following:
        //   - Get current time
        uint64_t currTime = currentTime(); 

        { //"fake" class for mutex
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
            for (i = 0; i < processes.size(); i++)
            {
                if ((currentTime() - start) >= processes[i]->getStartTime() && processes[i]->getState() == Process::State::NotStarted)
                {
                    processes[i]->setState(Process::State::Ready, currentTime());
                    shared_data->ready_queue.push_back(processes[i]);
                }
            }
            
            // update process times
            for(i = 0; i < processes.size(); i++){
                processes[i]->updateProcess(currentTime());
            }

            //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
            for (i = 0; i < processes.size(); i++)
            {
                int64_t timeDifference = currentTime() - processes[i]->getBurstStartTime();
                int64_t burstTime = processes[i]->getCurrentBurstTime(processes[i]->getCurrentBurst());

                if (processes[i]->getState() == Process::State::IO && (timeDifference >= burstTime))
                { 
                    processes[i]->setState(Process::State::Ready, currentTime());
                    processes[i]->setReadyQlastTime(currentTime());

                    shared_data->ready_queue.push_back(processes[i]);
                }
            }

            //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
            for (i = 0; i < processes.size(); i++)
            {
                if (processes[i]->getState() == Process::State::Running)
                {
                    //check if time slice expired or if newly ready process has a higher priority.
                    if (shared_data->algorithm == ScheduleAlgorithm::RR && (currentTime() - processes[i]->getBurstStartTime() >= shared_data->time_slice))
                    { //RR time slice expired
                        processes[i]->interrupt();
                    }
                    if (shared_data->algorithm == ScheduleAlgorithm::PP)
                    {
                        //loop throught newly ready processes and see if one of them has a higher priority
                        for (int j = 0; j < processes.size(); j++)
                        {
                            if (processes[j]->getPriority() < processes[i]->getPriority() && processes[j]->getState() == Process::State::Ready)
                            { //lower priority num == more important
                                processes[i]->interrupt();
                            }
                        }
                    }
                }
            }

            //   - *Sort the ready queue (if needed - based on scheduling algorithm)

            if (shared_data->algorithm == ScheduleAlgorithm::SJF)
            {
                shared_data->ready_queue.sort(SjfComparator());
            }
            if (shared_data->algorithm == ScheduleAlgorithm::PP)
            {
                shared_data->ready_queue.sort(PpComparator());
            }
        } //end of "fake" class for mutex

        //   - Determine if all processes are in the terminated state
        bool term = true;
        for (i = 0; i < processes.size(); i++)
        {
            if (!(processes[i]->getState() == Process::State::Terminated))
            {
                term = false; //there is a process that has a state other than terminated
            }
        }
        if (term)
        {
            shared_data->all_terminated = true;
        }
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization
        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);
        // sleep 50 ms
        usleep(50000);
    }

    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }


    //loop through processes to get the final statistics 

    double avgTurnaround = 0;
    double avgWait = 0;
    double throughputFirst = 0;
    double throughputSecond = 0;
    int firstCount = 0;;
    int secondCount = 0;
    double cpuUt = 0;

    for(i = 0; i < processes.size(); i++){
        avgTurnaround += processes[i]->getTurnaroundTime();
        avgWait += processes[i]->getWaitTime();
        cpuUt += (processes[i]->getCpuTime()/processes[i]->getTurnaroundTime());

        if(processes[i]->getTermNumber() < (processes.size()/2)) {
            throughputFirst += processes[i]->getTurnaroundTime();
            firstCount++;

        }else {
            throughputSecond += processes[i]->getTurnaroundTime();
            secondCount++;
        }
    }
    avgTurnaround = avgTurnaround/processes.size();
    avgWait = avgWait/processes.size();
    throughputFirst = throughputFirst / firstCount;
    throughputSecond = throughputSecond / secondCount;
    double throughputOverall = (throughputFirst + throughputSecond)/2;
    cpuUt = 1 - (cpuUt/processes.size());
    cpuUt = cpuUt * 100.0;


    // print final statistics
    //  - CPU utilization
    //  - Throughput
    //     - Average for first 50% of processes finished
    //     - Average for second 50% of processes finished
    //     - Overall average
    //  - Average turnaround time
    //  - Average waiting time
    if(processes.size() > 1){
        printf("\nFinal Statistics:\n - CPU utilization: \033[32m%.1f%%\033[0m\n - Throughput\n    - Average for first 50%% of processes finished: \033[32m%.1f seconds per process\033[0m\n    - Average for second 50%% of processes finished: \033[32m%.1f seconds per process\033[0m\n    - Overall average: \033[32m%.1f seconds per process\033[0m\n - Average turnaround time: \033[32m%.1f seconds\033[0m\n - Average waiting time: \033[32m%.1f seconds\033[0m\n", cpuUt, throughputFirst, throughputSecond, throughputOverall, avgTurnaround, avgWait);
    }else {
        printf("\nFinal Statistics:\n - CPU utilization: \033[32m%.1f%%\033[0m\n - Throughput\n    - Average for first 50%% of processes finished: \033[32m%.1f seconds per process\033[0m\n    - Overall average: \033[32m%.1f seconds per process\033[0m\n - Average turnaround time: \033[32m%.1f seconds\033[0m\n - Average waiting time: \033[32m%.1f seconds\033[0m\n", cpuUt, throughputFirst, throughputFirst, avgTurnaround, avgWait);
    }


    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    Process *p = NULL;
    while (!(shared_data->all_terminated))
    {
        uint64_t currTime = currentTime(); 
        if (p == NULL)
        {
            { //"fake" class for mutex
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                if(shared_data->ready_queue.size() > 0){
                    //   - *Get process at front of ready queue
                    p = shared_data->ready_queue.front();
                    shared_data->ready_queue.remove(shared_data->ready_queue.front());
                }

            } //end of "fake" class for mutex

            if(p != NULL){
                p->setRunningQlastTime(currentTime());
                p->setState(Process::State::Running, 0);
            
                p->setBurstStartTime(currentTime());

                if (p->getCurrentBurst() != 0 && !p->isInterrupted())
                {
                    p->setCurrentBurst(p->getCurrentBurst() + 1);
                }else {
                    p->interruptHandled();
                }

                p->setCpuCore(core_id);
            }
        }

        if (p != NULL)
        {
            int64_t timeDiff = currentTime() - p->getBurstStartTime();
            int16_t burstNum = p->getCurrentBurst();
            int16_t totalBursts = p->getNumberOfBursts();

            if (timeDiff >= p->getCurrentBurstTime(burstNum))
            { //check if CPU burst time has elapsed
                // Place the process back in the appropriate queue
                if(p->isInterrupted()){ //if there is an interrupt signal sent and the process happens to also finish its CPU burst at the same time
                    p->interruptHandled();
                }

                p->updateBurstTime(burstNum, 0);

                if (burstNum + 1 == totalBursts)
                { //no more bursts remain and current cpu burst finished --> set state to terminated
                    p->setState(Process::State::Terminated, 0);
                    {
                        std::lock_guard<std::mutex> lock(shared_data->mutex);
                        p->setTermNumber(shared_data->termNumber);
                        shared_data->termNumber++;
                    }
                    p->setCpuCore(-1);
                    p = NULL;
                }
                else
                {
                    // Setting process to IO";
                    p->setCurrentBurst(p->getCurrentBurst() + 1);
                    p->setState(Process::State::IO, 0);
                    p->setBurstStartTime(currentTime());
                    p->setCpuCore(-1);

                    p = NULL;
                }
            }

            if (p != NULL && p->isInterrupted())
            { //Interrupted (RR time slice has elapsed or process preempted by higher priority process)

                //update the burstTime 
                int64_t timeDiff = currentTime() - p->getBurstStartTime();
                uint32_t currBurstTime = p->getCurrentBurstTime(p->getCurrentBurst());

                currBurstTime = currBurstTime - timeDiff;

                p->updateBurstTime(p->getCurrentBurst(), currBurstTime);

                { //"fake" class for mutex
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    p->setReadyQlastTime(currentTime());
                    p->setRunningQlastTime(currentTime());
                    p->setState(Process::State::Ready, 0);
                    p->setCpuCore(-1);
                    shared_data->ready_queue.push_back(p);
                } //end of "fake" class for mutex
                p = NULL;
            }

            //   - Simulate the processes running until one of the following:
            //     - CPU burst time has elapsed
            //     - Interrupted (RR time slice has elapsed or process preempted by higher priority process)

            //  - Place the process back in the appropriate queue
            //     - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
            //     - Terminated if CPU burst finished and no more bursts remain -- no actual queue, simply set state to Terminated
            //     - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)

            //  - Wait context switching time
            usleep(shared_data->context_switch); //right? and need the other sleep too?
        }
        // sleep 50 ms
        //usleep(50000); 
        //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
    }
}

int printProcessOutput(std::vector<Process *> &processes, std::mutex &mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time | BURST NUM |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+-----------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            double currBurst = processes[i]->getCurrentBurst();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf | %11.1lf |\n",
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time,
                   wait_time, cpu_time, remain_time, currBurst);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
    case Process::State::NotStarted:
        str = "not started";
        break;
    case Process::State::Ready:
        str = "ready";
        break;
    case Process::State::Running:
        str = "running";
        break;
    case Process::State::IO:
        str = "i/o";
        break;
    case Process::State::Terminated:
        str = "terminated";
        break;
    default:
        str = "unknown";
        break;
    }
    return str;
}