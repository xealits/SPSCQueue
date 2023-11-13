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


#if __has_include(<boost/lockfree/spsc_queue.hpp> )
#include <boost/lockfree/spsc_queue.hpp>
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
#include <folly/ProducerConsumerQueue.h>
#endif

#define debug_logging 0
#include "test_parsing.h"

#define PARSE

//#define MEMCPY 0

#define MAX_CLUSTERS 1
#define MAX_ABCs    10

bool test_spscqueue = false;
bool test_boost     = false;

void pinThread(int cpu) {
  if (cpu < 0) {
    return;
  }
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
alignas(kCacheLineSize) unsigned long long n_all_payload_bytes = 0;
alignas(kCacheLineSize) unsigned long long all_res = 0; // dumy output

int64_t process_core(rigtorp::SPSCQueue<uint8_t>& q, struct FrontEndData* fe_data) {
  uint8_t* rawDataContainer_ptr = nullptr;
  int64_t n_packets_processed = 0;

  #if (debug_logging > 0)
    std::cout << "process_core\n";
  #endif

  while (rawDataContainer_ptr = q.front()) {
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
      parse_data(&rawData_ptr[1], n_packet_payload, fe_data);

      // TODO: printout the packets to check?
      #if (debug_logging > 0)
        print_FrontEndData(fe_data);
      #endif
      #endif

      n_all_payload_bytes += n_packet_payload; // account things
      n_all_payload += n_packet_payload+1; // the container packet + its flat size byte
      rawData_ptr += n_packet_payload+1;
    }

    //q.allocate_pop(*rawData_ptr);
    q.allocate_pop_n(n_all_payload+1); // + the flat size byte for the container size
    //std::cout << "the consumer i " << i << std::endl;
    n_packets_processed += n_packets;
  }
  
  return n_packets_processed;
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

  const int64_t n_containers  = 200000; // 100000;
  const int64_t n_raw_packets = 20; // 1; // per container
  const int64_t n_repeat      = 100;


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
    SPSCQueue<uint8_t> q(1024*8, 512);

/*
*/
    int64_t n_packets_processed = 0;
    auto t_consumer = std::thread([&] {
      pinThread(cpu1);
      
      std::cout << "running the consumer thread" << std::endl;
      // output data
      struct FrontEndData fe_data[2];
      //FrontEndHit  fe_hits_array[max_n_abcs*max_n_clusters*2];
      FrontEndHit  fe_hits_array[MAX_ABCs*MAX_CLUSTERS*2];
      // set up the output pad
      fe_data[0].l0id = 0;
      fe_data[0].bcid = 0;
      fe_data[0].n_hits  = 0;
      fe_data[0].fe_hits = fe_hits_array;


      //for (int i = 0; i < n_raw_packets*n_repeat; ++i)
      //while (n_packets_processed < n_containers*n_raw_packets*n_repeat)
      while (true)
      {
        q.waitNotEmptyOrDone(); // this is a blocking call
        // it guarantees that front() returns something and the following busy loop won't fire
      
        n_packets_processed += process_core(q, fe_data);

        #if (debug_logging > 0)
          std::cout << "process core after wait\n";
        #endif

        if (q.isDone()) {
          n_packets_processed += process_core(q, fe_data);

          #if (debug_logging > 0)
            std::cout << "process core after done\n";
          #endif

          break;
        }
      }

      std::cout << "the consumer is done, n packets: " << n_packets_processed << std::endl;
      if (n_packets_processed != n_containers*n_raw_packets*n_repeat)
        throw std::runtime_error("the consumer processed " + std::to_string(n_packets_processed) + " != " + std::to_string(n_containers*n_raw_packets*n_repeat));
    });

    pinThread(cpu2);

    auto start_setup = std::chrono::steady_clock::now();
    // generate the data in memory
    const static long long unsigned a_packet_size = 2 + MAX_ABCs * MAX_CLUSTERS * 2 + 2;
    const static long long unsigned n_max_raw_data_bytes = n_raw_packets * (2 + a_packet_size); // with the netio header
    // 2=netio header + (2=header + N_ABCs*N_CLUSTERs*2bytes + 2=footer)
    uint8_t raw_data[n_max_raw_data_bytes];
    
    auto raw_data_ptr = &raw_data[0];
    //for (int rep_i = 0; rep_i < n_repeat; ++rep_i) {
    //}

    for (int i = 0; i < n_raw_packets; ++i) {
      //q.emplace(i);
      // TODO: pre-known size!
      myBool with_netio_header = myFalse;
      #ifdef MEMCPY
      with_netio_header = myTrue;
      #endif
      size_t n_bytes = (MAX_CLUSTERS*MAX_ABCs*2 + 2 + 2) + (with_netio_header? 2 : 0);
      // the last 2 is the netio header -- it should not be there
      // (and there used to be 1 for the flat size byte)

      auto n_bytes_filled = fill_generated_data(raw_data_ptr, myFalse, myFalse, MAX_CLUSTERS, MAX_ABCs, with_netio_header);
      #ifdef debug_logging
      if (n_bytes_filled != n_bytes) throw std::runtime_error("wrong n_bytes_filled! " + std::to_string(n_bytes_filled) + " != " + std::to_string(n_bytes));
      #endif
      raw_data_ptr+=n_bytes;
    }

    // a single packet pad
    uint8_t raw_a_packet[a_packet_size];
    auto n_bytes_filled = fill_generated_data(raw_a_packet, myFalse, myFalse, MAX_CLUSTERS, MAX_ABCs, myFalse);
    if (n_bytes_filled != a_packet_size) throw std::runtime_error("a_packet wrong n_bytes_filled! " + std::to_string(n_bytes_filled) + " != " + std::to_string(a_packet_size));

    auto stop_setup = std::chrono::steady_clock::now();

    std::cout << "setup time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stop_setup - start_setup).count() << " ms" << std::endl;

    auto start = std::chrono::steady_clock::now();

/*
*/
    // producer pushes the raw data to the queue:

    uint8_t  n_packets_in_current_container = 0;
    uint8_t  n_previous_packet_payload = 0; // = offset to the next flat packet place in the buffer
    uint8_t* curr_container_size_byte_ptr = nullptr;
    uint8_t* rawData_ptr = nullptr;
    for (int rep_i = 0; rep_i < n_repeat; ++rep_i) {
      raw_data_ptr = &raw_data[0];
      //for (int i = 0; i < n_raw_packets; ++i)

      for (int i = 0; i < n_containers*n_raw_packets; ++i)
      {
        //q.emplace(i);
        uint8_t n_bytes  = raw_data_ptr[1];

        n_bytes = 2 + MAX_ABCs * MAX_CLUSTERS * 2 + 2; // not randomized

        // allocation logic
        // nested
        if (n_packets_in_current_container==0) { // then it's a new container
          // allocate the new flat container with 1 packet
          curr_container_size_byte_ptr = q.allocate_n(n_bytes+1+1); // +1 flat size byte for the packet and the container
          rawData_ptr = &curr_container_size_byte_ptr[1];
          n_packets_in_current_container = 1;
          n_previous_packet_payload = n_bytes+1;
        }

        else { // the container exists, try to extend it
          int extend_status = q.allocate_extend(n_bytes+1); // packet size + 1 flat size byte
          if (extend_status == 0) {
            // extend failed, store and allocate new container
            curr_container_size_byte_ptr[0] = n_packets_in_current_container; // save the container size
            q.allocate_store();
            #if debug_logging > 1
            std::cout << "push thread: allocate_store on failed extend\n";
            #endif

            // new container
            curr_container_size_byte_ptr = q.allocate_n(n_bytes+1+1);
            rawData_ptr = &curr_container_size_byte_ptr[1];
            n_packets_in_current_container = 1;
            n_previous_packet_payload = n_bytes+1;
          }

          else {
            // successfull extension
            n_packets_in_current_container += 1;
            rawData_ptr += n_previous_packet_payload;
            n_previous_packet_payload = n_bytes+1;
          }
        }

        // at this point I have a valid rawData_ptr ?
        rawData_ptr[0] = n_bytes;
        // rawData_ptr[1] can get payload

        // copy the data into the queue
        #ifdef MEMCPY

        #if debug_logging > 0
        uint8_t elink_id = raw_data_ptr[0]; // not used here
        std::cout << "push on elink=" << (unsigned) elink_id << " n_bytes=" << (unsigned) n_bytes << "\n";
        #endif

        //// 2 is the netio header -- it should not be there
        //// 1 is the flat byte
        //auto rawData_ptr = q.allocate_n(n_bytes+1); // +1 flat size byte
        //// TODO the user has to set it manually:
        //rawData_ptr[0] = n_bytes;

        #if MEMCPY > 0
        memcpy(&rawData_ptr[1], raw_a_packet, n_bytes*sizeof(uint8_t));
        #else
        memcpy(&rawData_ptr[1], &raw_data_ptr[2], n_bytes*sizeof(uint8_t));
        #endif
        #else
        // direct fill
        auto n_bytes_filled = fill_generated_data(&rawData_ptr[1], myFalse, myFalse, MAX_CLUSTERS, MAX_ABCs, myFalse);
        #if debug_logging > 2
        std::cout << "push directly to the queue n_bytes_filled=" << n_bytes_filled << "\n";
        #endif
        #endif

        // if reached max container size -- store
        if (n_packets_in_current_container==n_raw_packets) {
          curr_container_size_byte_ptr[0] = n_packets_in_current_container;
          q.allocate_store();
          n_packets_in_current_container = 0;
          n_previous_packet_payload = 0;
          #if debug_logging > 1
          std::cout << "push thread: allocate_store on max packets in container\n";
          #endif
        }
        ////auto allocation_shift = allocation.allocateNextWriteIdxCache_;
        ////q.allocate_store(allocation_shift);
        //q.allocate_store();
        //rawData_ptr += n_bytes+1;
      }
    }

    // if something is left to store:
    if (n_packets_in_current_container!=0) {
      curr_container_size_byte_ptr[0] = n_packets_in_current_container;
      q.allocate_store(); // TODO: can it execute these two lines out of order?
      #if debug_logging > 1
      std::cout << "push thread: allocate_store remainder\n";
      #endif
    }

    q.finish(); // signal that the input is done

    t_consumer.join();

    auto stop = std::chrono::steady_clock::now();

    std::cout << "all res =" << all_res << std::endl;

    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count() << " ms" << std::endl;

    //std::cout << n_repeat * n_containers * n_raw_packets * 1000000 /
    // there is a test that n packets processed = the three multipliers
    std::cout << n_packets_processed * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;

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
