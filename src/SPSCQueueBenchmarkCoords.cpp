/*
Copyright (c) 2018 Erik Rigtorp <erik@rigtorp.se>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#include <chrono>
#include <iostream>
#include <rigtorp/SPSCQueue.h>
#include <thread>

#include <sys/mman.h>

#include <x86intrin.h> // unsigned long long __rdtsc();

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
#include <boost/lockfree/spsc_queue.hpp>
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
#include <folly/ProducerConsumerQueue.h>
#endif

#define debug_logging 0
#include "test_parsing.h"

#define N_PROCS  2
#define N_ELINKS 8

#define RECORD_PROC_TIMINGS
//#define RECORD_MEMCPY_TIMINGS

#define WITH_ELINK_ID

#ifdef WITH_ELINK_ID
#define FLAT_PACKET_HEADER_SIZE 2
#else
#define FLAT_PACKET_HEADER_SIZE 1
#endif

#define FLAT_CONTAINER_HEADER_SIZE 1 // 1 byte for N packets in the container

#define PARSE

#define MEMCPY 0

#define MAX_RAWDATACONTAINER_SIZE (1024+512) // in bytes, must be >= 1 as there is always 1 flat size byte

#define MAX_CLUSTERS 1
#define MAX_ABCs    10

bool test_spscqueue = false;
bool test_boost     = false;

void pinThread(int cpu) {
  if (cpu < 0) {
    return;
  }
  std::cout << "pinThread " + std::to_string(cpu) + "\n";

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) ==
      -1) {
    perror("pthread_setaffinity_no");
    exit(1);
  }
}


template <typename T> struct Allocator {
  using value_type = T;

  struct AllocationResult {
    T *ptr;
    size_t count;
  };

  size_t roundup(size_t n) { return (((n - 1) >> 21) + 1) << 21; }

  AllocationResult allocate_at_least(size_t n) {
    size_t count = roundup(sizeof(T) * n);
    auto p = static_cast<T *>(mmap(nullptr, count, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                                   -1, 0));
    if (p == MAP_FAILED) {
      throw std::bad_alloc();
    }
    return {p, count / sizeof(T)};
  }

  void deallocate(T *p, size_t n) { munmap(p, roundup(sizeof(T) * n)); }
};


static constexpr size_t kCacheLineSize = 64;
//alignas(kCacheLineSize) unsigned long long n_all_payload_bytes = 0;
alignas(kCacheLineSize) unsigned long long all_res = 0; // dumy output

struct nPacketsBytes {
  uint64_t n_packets=0;
  uint64_t n_bytes=0;
  unsigned long long t_diff = 0;
};

struct nPacketsBytes process_core(rigtorp::SPSCQueue<uint8_t>& q, struct FrontEndData* fe_data, uint8_t proc_id) {
  uint8_t* rawDataContainer_ptr = nullptr;
  alignas(kCacheLineSize) uint64_t n_packets_processed = 0;
  alignas(kCacheLineSize) uint64_t n_all_payload_bytes = 0;

  #if (debug_logging > 0)
    std::cout << "process_core\n";
  #endif

  unsigned long long __time_proc_start = 0, __time_proc_end = 0;
  #ifdef RECORD_PROC_TIMINGS
  __time_proc_start = __rdtsc();
  #endif

  while ((rawDataContainer_ptr = q.front())) {
    //while (!q.front())
    //  ;
    //if (*q.front() != i) {
    //  throw std::runtime_error("");
    //}
    //auto rawData_ptr = q.allocate_front();
    //uint8_t n_payload = 24; // rawData_ptr->ptr[1];
    //auto rawDataContainer_ptr = q.front();
    uint8_t n_packets = rawDataContainer_ptr[0]; // in case of containers, first byte = n packets in it
    size_t n_all_payload = 0;

    #if (debug_logging > 0)
      std::cout << "new raw data container n_packets= " + std::to_string((unsigned) n_packets) << "\n";
    #endif

    uint8_t* rawData_ptr = &rawDataContainer_ptr[1];
    for (unsigned packet_i=0; packet_i<n_packets; packet_i++) {
      #ifdef PARSE
      //parse_data;
      //uint8_t elink_id  = rawData_ptr[1]; // not anymore! just raw_data

      uint8_t n_packet_payload = rawData_ptr[0];

      ////parse_data(&rawData_ptr->ptr[2], n_payload, fe_data);
      //unsigned res = parse_data(&rawData_ptr[1], n_payload, fe_data);
      //all_res += parse_data(&rawData_ptr[1], n_packet_payload, fe_data);
      
      #ifdef WITH_ELINK_ID
      uint8_t elink_id = rawData_ptr[1];
      parse_data_elinks(&rawData_ptr[2], n_packet_payload, elink_id, fe_data);
      #else
      parse_data(&rawData_ptr[1], n_packet_payload, fe_data);
      #endif

      // TODO: printout the packets to check?
      #if (debug_logging > 0)
        print_FrontEndData(fe_data, elink_id, proc_id);
      #endif
      #endif

      n_all_payload_bytes += n_packet_payload; // account things
      n_all_payload += n_packet_payload+FLAT_PACKET_HEADER_SIZE; // the container packet + its flat size byte
      rawData_ptr += n_packet_payload+FLAT_PACKET_HEADER_SIZE;
    }

    //q.allocate_pop(*rawData_ptr);
    q.allocate_pop_n(n_all_payload+1); // + the flat size byte for the container size
    //std::cout << "the consumer i " << i << std::endl;
    n_packets_processed += n_packets;
  }

  #ifdef RECORD_PROC_TIMINGS
  __time_proc_end = __rdtsc();
  #endif

  return {.n_packets=n_packets_processed, .n_bytes=n_all_payload_bytes, .t_diff=__time_proc_end - __time_proc_start};
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  using namespace rigtorp;

  int cpu1 = -1;
  int cpu2 = -1;

  if (argc == 3) {
    cpu1 = std::stoi(argv[1]);
    cpu2 = std::stoi(argv[2]);
  }

  const size_t queueSize = 10000000;
  const int64_t iters = 1000000; // this becomes too large to reserve the buffers for raw and fe data etc

  const int64_t n_containers  = 5000; //10000; //200000; // 100000;
  const int64_t n_raw_packets = 60; // 1; // per container
  const int64_t n_repeat      = 1000;


  if (test_spscqueue) {
    std::cout << "SPSCQueue:" << std::endl;
    SPSCQueue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int i = 0; i < iters; ++i) {
        while (!q.front())
          ;
        if (*q.front() != i) {
          throw std::runtime_error("");
        }
        q.pop();
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
      q.emplace(i);
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  if (test_spscqueue) {
    std::cout << "SPSCQueue RTT:" << std::endl;
    SPSCQueue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int i = 0; i < iters; ++i) {
        while (!q1.front())
          ;
        q2.emplace(*q1.front());
        q1.pop();
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
      q1.emplace(i);
      while (!q2.front())
        ;
      q2.pop();
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }

  std::cout << "SPSCQueueCoords:" << std::endl;
  {
    //SPSCQueueCoord<uint8_t> q(512, 1024); // 512 bytes, l1 cache line is 64 bytes, typical packet size is 24-44 bytes
    //SPSCQueue<uint8_t, Allocator<uint8_t>> q(1024*1, 128); // huge pages allocator does not work: what():  std::bad_alloc
    //SPSCQueue<uint8_t> q(1024*3, 128);
    //SPSCQueue<uint8_t> q(1024*8, 512);
    //std::vector<SPSCQueue<uint8_t>> rawDataQueues; // this did not work -- queue is not copiable
    //rawDataQueues.reserve(N_PROCS);
    //SPSCQueue<uint8_t> rawDataQueues[1] = {{1024*8, 512}};

    std::vector<SPSCQueue<uint8_t>*> rawDataQueues;
    rawDataQueues.reserve(N_PROCS);

    std::vector<std::thread> rawDataThreads;
    rawDataThreads.reserve(N_PROCS);

    std::vector<uint64_t> nPacketsProcessed;
    nPacketsProcessed.reserve(N_PROCS);
    std::vector<uint64_t> nBytesProcessed;
    nBytesProcessed.reserve(N_PROCS);
    std::vector<unsigned long long> nProcessingTime;
    nProcessingTime.reserve(N_PROCS);
    std::vector<unsigned long long> nProcessCore;
    nProcessCore.reserve(N_PROCS);

    for (unsigned proc_i=0; proc_i<N_PROCS; proc_i++) {
      //rawDataQueues.push_back(SPSCQueue<uint8_t>(1024*8, 512));
      //rawDataQueues[proc_i] = SPSCQueue<uint8_t>(1024*8, 512);

      rawDataQueues.push_back(new SPSCQueue<uint8_t>(1024*8, MAX_RAWDATACONTAINER_SIZE));
      //int64_t n_packets_processed = 0;
      nPacketsProcessed.push_back(0);
      nBytesProcessed.push_back(0);
      nProcessingTime.push_back(0);
      nProcessCore.push_back(0);

      //auto t_consumer = std::thread([&]
      rawDataThreads.push_back(std::thread([&] (unsigned thread_index, int cpu_to_pin)
      {
        pinThread(cpu_to_pin);

        auto& q = *(rawDataQueues[thread_index]);
        auto& n_packets_processed = nPacketsProcessed[thread_index];

        uint64_t           _nPacketsProcessed = 0;
        uint64_t           _nBytesProcessed   = 0;
        unsigned long long _nProcessingTime   = 0;
        unsigned long long _nProcessCore   = 0;

        std::cout << "running the consumer thread\n";
        // output data
        struct FrontEndData fe_data[2]; // TODO outdata pad: N elinks & some efficient map/switch?
        //FrontEndHit  fe_hits_array[max_n_abcs*max_n_clusters*2];
        FrontEndHit  fe_hits_array[MAX_ABCs*MAX_CLUSTERS*2];
        // set up the output pad
        fe_data[0].l0id = 0;
        fe_data[0].bcid = 0;
        fe_data[0].n_hits  = 0;
        fe_data[0].fe_hits = fe_hits_array;

        fe_data[1].l0id = 0;
        fe_data[1].bcid = 0;
        fe_data[1].n_hits  = 0;
        fe_data[1].fe_hits = &fe_hits_array[MAX_ABCs*MAX_CLUSTERS];

        //for (int i = 0; i < n_raw_packets*n_repeat; ++i)
        //while (n_packets_processed < n_containers*n_raw_packets*n_repeat)
        while (true)
        {
          q.waitNotEmptyOrDone(); // this is a blocking call
          // it guarantees that front() returns something and the following busy loop won't fire
        
          struct nPacketsBytes stats = process_core(q, fe_data, thread_index);
          _nPacketsProcessed  += stats.n_packets;
          _nBytesProcessed    += stats.n_bytes;
          _nProcessingTime    += stats.t_diff;
          _nProcessCore ++;

          #if (debug_logging > 0)
            std::cout << "process core after wait\n";
          #endif

          if (q.isDone()) {
            stats = process_core(q, fe_data, thread_index);
            _nPacketsProcessed += stats.n_packets;
            _nBytesProcessed   += stats.n_bytes;
            _nProcessingTime   += stats.t_diff;
            _nProcessCore ++;

            #if (debug_logging > 0)
              std::cout << "process core after done\n";
            #endif

            nPacketsProcessed [thread_index] = _nPacketsProcessed ;
            nBytesProcessed   [thread_index] = _nBytesProcessed   ;
            nProcessingTime   [thread_index] = _nProcessingTime   ;
            nProcessCore      [thread_index] = _nProcessCore   ;
            break;
          }
        }

        std::cout << "the consumer is done, n_packets=" + std::to_string(nPacketsProcessed[thread_index]) + 
                     " n_bytes=" + std::to_string(nBytesProcessed[thread_index]) + "\n";
        
        //// nope, we are not copying anymore
        //if (n_packets_processed != n_containers*n_raw_packets*n_repeat)
        //  throw std::runtime_error("the consumer processed " + std::to_string(n_packets_processed) + " != " + std::to_string(n_containers*n_raw_packets*n_repeat));
      }, proc_i, cpu2));

      if (cpu2 > 0) { cpu2++; }
    }

    pinThread(cpu1);

    auto start_setup = std::chrono::steady_clock::now();
    // generate the data in memory
    const static long long unsigned a_packet_size = 2 + MAX_ABCs * MAX_CLUSTERS * 2 + 2;
    const static long long unsigned n_max_raw_data_bytes = n_containers * n_raw_packets * (2 + a_packet_size); // with the netio header
    // 2=netio header + (2=header + N_ABCs*N_CLUSTERs*2bytes + 2=footer)
    uint8_t raw_data[n_max_raw_data_bytes];
    
    auto raw_data_ptr = &raw_data[0];
    //for (int rep_i = 0; rep_i < n_repeat; ++rep_i) {
    //}

    uint8_t elink_id = 0;
    for (int i = 0; i < n_containers * n_raw_packets; ++i) {
      //q.emplace(i);
      // TODO: pre-known size!
      
      #ifdef WITH_ELINK_ID
      myBool with_netio_header = myTrue;
      #else
      myBool with_netio_header = myFalse;
      #endif

      //#ifdef MEMCPY
      //with_netio_header = myTrue; // not exactly used yet
      //#endif
      size_t n_bytes = (MAX_CLUSTERS*MAX_ABCs*2 + 2 + 2) + (with_netio_header? 2 : 0);
      // the last 2 is the netio header -- it should not be there
      // (and there used to be 1 for the flat size byte)

      auto n_bytes_filled = fill_generated_data(raw_data_ptr, myFalse, myFalse, MAX_CLUSTERS, MAX_ABCs, with_netio_header, elink_id);

      #ifdef debug_logging
      if (n_bytes_filled != n_bytes) throw std::runtime_error("wrong n_bytes_filled! " + std::to_string(n_bytes_filled) + " != " + std::to_string(n_bytes));
      #endif
      raw_data_ptr+=n_bytes;
    
      elink_id++;
      if (elink_id >= N_ELINKS) elink_id = 0;
    }

    // a single packet pad
    uint8_t raw_a_packet[a_packet_size];
    auto n_bytes_filled = fill_generated_data(raw_a_packet, myFalse, myFalse, MAX_CLUSTERS, MAX_ABCs, myFalse);
    if (n_bytes_filled != a_packet_size) throw std::runtime_error("a_packet wrong n_bytes_filled! " + std::to_string(n_bytes_filled) + " != " + std::to_string(a_packet_size));

    auto stop_setup = std::chrono::steady_clock::now();

    std::cout << "setup time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stop_setup - start_setup).count() << " ms" << std::endl;

    // the push thread pushPad info
    /*
    uint8_t  n_packets_in_current_container_s[N_PROCS];
    uint8_t  n_previous_packet_payload_s[N_PROCS]; // = offset to the next flat packet place in the buffer
    uint8_t* curr_container_size_byte_ptr_s[N_PROCS];
    uint8_t* rawData_ptr_s[N_PROCS];
    */

    uint8_t  pushPad[N_PROCS][MAX_RAWDATACONTAINER_SIZE];
    unsigned pushPad_curIndex[N_PROCS];
    alignas(64) uint8_t  pushPad_nPackets[N_PROCS];
    // a local cache of the flat nPackets bytes, so that it does not walk the addresses too much

    alignas(64) unsigned long long time_memcpy_pushPad  = 0;
    alignas(64) unsigned long long time_memcpy_allocate = 0;

    for (unsigned proc_i=0; proc_i<N_PROCS; proc_i++) {
        //n_packets_in_current_container_s[proc_i] = 0;
        //n_previous_packet_payload_s[proc_i] = 0;
        //curr_container_size_byte_ptr_s[proc_i] = nullptr;
        //rawData_ptr_s[proc_i] = nullptr;
        pushPad[proc_i][0] = 0; // container size
        pushPad_nPackets[proc_i] = 0;
        pushPad_curIndex[proc_i] = FLAT_CONTAINER_HEADER_SIZE;
    }

    // producer pushes the raw data to the queue:
    auto start = std::chrono::steady_clock::now();

/*
*/
    unsigned long long __time_push_start = __rdtsc();
    for (int rep_i = 0; rep_i < n_repeat; ++rep_i) {
      raw_data_ptr = &raw_data[0];
      //for (int i = 0; i < n_raw_packets; ++i)

      uint8_t n_bytes  = 0; // n_bytes in the current raw data packet that is repeated N times

      for (int i = 0; i < n_containers*n_raw_packets; ++i)
      {
        // push the same data to each processing worker
        //for (unsigned proc_i=0; proc_i<N_ELINKS; proc_i++)
        {
        //q.emplace(i);
        uint8_t elink_id = raw_data_ptr[0]; // not used here TODO: start using elink id, so that it does not unfold the loop etc
        n_bytes  = raw_data_ptr[1];

        n_bytes = 2 + MAX_ABCs * MAX_CLUSTERS * 2 + 2; // not randomized

        // 1 = container flat size byte (for n packets)
        // 1 = packet flat size byte (for n bytes in the packet)
        // n_bytes = packet RawData payload
        if (1+FLAT_PACKET_HEADER_SIZE+n_bytes > MAX_RAWDATACONTAINER_SIZE) { // it won't fit
          throw std::runtime_error("raw packet data won't fit into the max size container: " + std::to_string((unsigned)1+FLAT_PACKET_HEADER_SIZE+n_bytes) + std::to_string((unsigned) MAX_RAWDATACONTAINER_SIZE));
        }

        unsigned proc_i = 0;

/*
        switch (elink_id) {
          case 0:
          case 1:
            proc_i = 0;
            break;
          case 2:
          case 3:
            proc_i = 1;
            break;
          case 4:
          case 5:
            proc_i = 2;
            break;
          case 6:
          case 7:
            proc_i = 3;
            break;
          case 8:
          case 9:
            proc_i = 4;
            break;
          case 10:
          case 11:
            proc_i = 5;
            break;
        }
*/

        switch (elink_id) {
          case 0:
          case 1:
          case 2:
          case 3:
            proc_i = 0;
            break;
          case 4:
          case 5:
          case 6:
          case 7:
            proc_i = 1;
            break;
          case 8:
          case 9:
          case 10:
          case 11:
            proc_i = 2;
            break;
        }

        //auto& n_packets_in_current_container = pushPad[proc_i][0];
        auto& n_packets_in_current_container = pushPad_nPackets[proc_i];
        auto& curIndex = pushPad_curIndex[proc_i];

        // if the new data does not fit, then push the current data to the queue
        // and clear the pushPad
        // the new packet is n_bytes long
        // but +1 is its flat size byte
        if (curIndex+FLAT_PACKET_HEADER_SIZE+n_bytes > MAX_RAWDATACONTAINER_SIZE) {
          // curIndex == the current number of all payload bytes in the pushPad
          auto& q = *(rawDataQueues[proc_i]);
          uint8_t* queuedContainer_ptr = q.allocate_n(curIndex);
          // save the n packets
          pushPad[proc_i][0] = n_packets_in_current_container;

          #if debug_logging > 1
          std::cout << "reached container size limit, push it: " + std::to_string(n_packets_in_current_container) + "\n";
          #endif

          // and memcpy to the queued space
          #if MEMCPY > 0
          //memcpy(&rawData_ptr[1], raw_a_packet, n_bytes*sizeof(uint8_t));
          #else
          memcpy(queuedContainer_ptr, pushPad[proc_i], curIndex*sizeof(uint8_t));
          #endif
          q.allocate_store();

          // clear the pushPad:
          n_packets_in_current_container = 0;
          curIndex = FLAT_CONTAINER_HEADER_SIZE;
        }

        // at this point I do have an index into the pushPad with enough space for the raw data
        // memcpy the data into it and set the flat size byte
        pushPad[proc_i][curIndex+0] = n_bytes;
        #ifdef WITH_ELINK_ID
        pushPad[proc_i][curIndex+1] = elink_id;
        #endif 

        #if MEMCPY > 0
        //memcpy(&rawData_ptr[1], raw_a_packet, n_bytes*sizeof(uint8_t)); // TODO this is outdated, right?
        #else

        #ifdef RECORD_MEMCPY_TIMINGS
        auto __time_memcpy_pushPad_start = __rdtsc();
        #endif
        memcpy(&pushPad[proc_i][curIndex+FLAT_PACKET_HEADER_SIZE], &raw_data_ptr[2], n_bytes*sizeof(uint8_t));

        #ifdef RECORD_MEMCPY_TIMINGS
        auto __time_memcpy_pushPad_end   = __rdtsc();
        time_memcpy_pushPad += __time_memcpy_pushPad_end - __time_memcpy_pushPad_start;
        #endif
        #endif

        curIndex += FLAT_PACKET_HEADER_SIZE+n_bytes;
        n_packets_in_current_container ++;

        // if n packets reached its max, push to the queue
        if (n_packets_in_current_container == n_raw_packets) {
          auto& q = *(rawDataQueues[proc_i]);
          uint8_t* queuedContainer_ptr = q.allocate_n(curIndex);

          // save the n packets
          pushPad[proc_i][0] = n_packets_in_current_container;

          #if debug_logging > 1
          std::cout << "reached N packets per container limit, push it: " + std::to_string(n_packets_in_current_container) + "\n";
          #endif

          // and memcpy to the queue
          #if MEMCPY > 0
          //memcpy(&rawData_ptr[1], raw_a_packet, n_bytes*sizeof(uint8_t));

          #else
          #ifdef RECORD_MEMCPY_TIMINGS
          auto __time_memcpy_start = __rdtsc();
          #endif

          memcpy(queuedContainer_ptr, pushPad[proc_i], curIndex*sizeof(uint8_t));
          #ifdef RECORD_MEMCPY_TIMINGS
          auto __time_memcpy_end   = __rdtsc();
          time_memcpy_allocate += __time_memcpy_end - __time_memcpy_start;
          #endif
          #endif
          q.allocate_store();

          // clear the pushPad:
          n_packets_in_current_container = 0;
          curIndex = FLAT_CONTAINER_HEADER_SIZE;
        }

      }
      raw_data_ptr += 2+n_bytes;

      //std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    // if something is left to store:
    for (unsigned proc_i=0; proc_i<N_PROCS; proc_i++) {
      //auto& n_packets_in_current_container = n_packets_in_current_container_s[proc_i];
      //auto& n_previous_packet_payload      = n_previous_packet_payload_s[proc_i];
      //auto& curr_container_size_byte_ptr   = curr_container_size_byte_ptr_s[proc_i];
      //auto& rawData_ptr = rawData_ptr_s[proc_i];

      //if (n_packets_in_current_container!=0) {
      //  curr_container_size_byte_ptr[0] = n_packets_in_current_container;
      //  //unsigned thread_index = 0;
      //  auto& q = *(rawDataQueues[proc_i]);
      //  q.allocate_store(); // TODO: can it execute these two lines out of order?
      //  #if debug_logging > 1
      //  std::cout << "push thread: allocate_store remainder\n";
      //  #endif
      //}

      //
      //auto& n_packets_in_current_container = pushPad[proc_i][0];
      auto& n_packets_in_current_container = pushPad[proc_i][0];
      pushPad[proc_i][0] = pushPad_nPackets[proc_i];
      auto& curIndex = pushPad_curIndex[proc_i];

      if (curIndex>1) {
        auto& q = *(rawDataQueues[proc_i]);
        uint8_t* queuedContainer_ptr = q.allocate_n(curIndex);
        #if MEMCPY > 0
        //memcpy(&rawData_ptr[1], raw_a_packet, n_bytes*sizeof(uint8_t));
        #else
        memcpy(queuedContainer_ptr, pushPad[proc_i], curIndex*sizeof(uint8_t));
        #endif
        q.allocate_store();

        n_packets_in_current_container = 0;
        curIndex = 1;
      }
    }

    unsigned long long __time_push_end = __rdtsc();

    #if debug_logging > 1
    std::cout << "push thread: finishing " + std::to_string(rawDataQueues.size()) + "\n";
    #endif
    //rawDataQueues[0]->finish();
    for (auto& q: rawDataQueues) {
      q->finish(); // signal that the input is done
      #if debug_logging > 1
      std::cout << "push thread: sent finish\n";
      #endif
    }

    #if debug_logging > 1
    std::cout << "push thread: joining threads!\n";
    #endif
    //t_consumer.join();
    //std::vector<std::thread> rawDataThreads;

    uint64_t n_packets_processed = 0;
    uint64_t n_all_payload_bytes = 0;
    for (unsigned proc_i=0; proc_i<rawDataThreads.size(); proc_i++) {
      //procThread.join();
      rawDataThreads[proc_i].join();
      //
      n_packets_processed += nPacketsProcessed[proc_i];
      n_all_payload_bytes += nBytesProcessed[proc_i];
    }

    n_packets_processed = nPacketsProcessed[0]; // overwrite, take just 1 processor

    unsigned long long __time_push_join = __rdtsc();

    auto stop = std::chrono::steady_clock::now();

    std::cout << "all res =" << all_res << std::endl;

    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count() << " ms" << std::endl;

    std::cout << "CPU time to push=" << std::to_string(__time_push_end  - __time_push_start)
              << " time to join="    << std::to_string(__time_push_join - __time_push_end) << std::endl;

    std::cout << "CPU time to memcpy pushPad="    << std::to_string(time_memcpy_pushPad)
              << " time to memcpy allocateQueue=" << std::to_string(time_memcpy_allocate) << std::endl;

    for (unsigned proc_i=0; proc_i<N_PROCS; proc_i++) {
      std::cout << "CPU time to process=" << std::to_string(nProcessingTime[proc_i]) << std::endl;
    }

    for (unsigned proc_i=0; proc_i<N_PROCS; proc_i++) {
      std::cout << "CPU n calls processCore=" << std::to_string(nProcessCore[proc_i]) << std::endl;
    }

    //std::cout << n_repeat * n_containers * n_raw_packets * 1000000 /
    // there is a test that n packets processed = the three multipliers
    std::cout << (n_packets_processed * N_PROCS / N_ELINKS) * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
              << " ops/ms (per elink)" << std::endl;

    std::cout << n_all_payload_bytes << " bytes" << std::endl;
    std::cout << (double) n_all_payload_bytes * 1000000000 / ((unsigned long long) 1000000 *
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count())
              << " MB/s" << std::endl;
  }

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
  if (test_boost) {
    std::cout << "boost::lockfree::spsc:" << std::endl;
    boost::lockfree::spsc_queue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int i = 0; i < iters; ++i) {
        int val;
        while (q.pop(&val, 1) != 1)
          ;
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
      while (!q.push(i))
        ;
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  if (test_boost) {
    std::cout << "boost::lockfree::spsc: RTT" << std::endl;
    boost::lockfree::spsc_queue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int i = 0; i < iters; ++i) {
        int val;
        while (q1.pop(&val, 1) != 1)
          ;
        while (!q2.push(val))
          ;
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
      while (!q1.push(i))
        ;
      int val;
      while (q2.pop(&val, 1) != 1)
        ;
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
  std::cout << "folly::ProducerConsumerQueue:" << std::endl;

  {
    folly::ProducerConsumerQueue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int i = 0; i < iters; ++i) {
        int val;
        while (!q.read(val))
          ;
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
      while (!q.write(i))
        ;
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    folly::ProducerConsumerQueue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int i = 0; i < iters; ++i) {
        int val;
        while (!q1.read(val))
          ;
        q2.write(val);
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
      while (!q1.write(i))
        ;
      int val;
      while (!q2.read(val))
        ;
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

  return 0;
}
