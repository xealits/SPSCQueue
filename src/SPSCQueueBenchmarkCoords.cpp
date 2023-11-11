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

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
#include <boost/lockfree/spsc_queue.hpp>
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
#include <folly/ProducerConsumerQueue.h>
#endif

#define debug_logging 0
#include "test_parsing.h"

#define PARSE

//#define MEMCPY 2

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
  const int64_t n_raw_packets = 100000;
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
    SPSCQueue<uint8_t> q(1024, 128);
    unsigned long long n_all_payload_bytes = 0;

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


      for (int i = 0; i < n_raw_packets*n_repeat; ++i) {
        while (!q.front())
          ;
        //if (*q.front() != i) {
        //  throw std::runtime_error("");
        //}
        //auto rawData_ptr = q.allocate_front();
        //uint8_t n_payload = 24; // rawData_ptr->ptr[1];
        auto rawData_ptr = q.front();
        uint8_t n_payload = rawData_ptr[0]; // the buffer the payload includes netio header
        #if (debug_logging > 0)
          std::cout << "new raw data n_payload=" << (unsigned) n_payload << "\n";
        #endif

        #ifdef PARSE
        //parse_data;
        //uint8_t elink_id  = rawData_ptr[1]; // not anymore! just raw_data

        //parse_data(&rawData_ptr->ptr[2], n_payload, fe_data);
        parse_data(&rawData_ptr[1], n_payload, fe_data);

        // TODO: printout the packets to check?
        #if (debug_logging > 0)
          print_FrontEndData(fe_data);
        #endif
        #endif

        n_all_payload_bytes += n_payload; // account things
        //q.allocate_pop(*rawData_ptr);
        q.allocate_pop_n(n_payload+1); // + the flat size byte
        //std::cout << "the consumer i " << i << std::endl;
      }

      std::cout << "the consumer is done" << std::endl;
    });

    pinThread(cpu2);

    auto start_setup = std::chrono::steady_clock::now();
    // generate the data in memory
    const static long long unsigned a_packet_size = 2 + MAX_ABCs * MAX_CLUSTERS * 2 + 2;
    const static long long unsigned n_max_raw_data_bytes = n_raw_packets * (2 + a_packet_size); // with the netio header
    // 2=netio header + (2=header + N_ABCs*N_CLUSTERs*2bytes + 2=footer)
    uint8_t raw_data[n_max_raw_data_bytes];
    auto raw_data_ptr = &raw_data[0];

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

    // producer pushes the raw data to the queue:
    for (int rep_i = 0; rep_i < n_repeat; ++rep_i) {
      raw_data_ptr = &raw_data[0];
      for (int i = 0; i < n_raw_packets; ++i) {
        //q.emplace(i);

        // copy the data into the queue
        #ifdef MEMCPY
        uint8_t n_bytes  = raw_data_ptr[1];

        #if debug_logging > 0
        uint8_t elink_id = raw_data_ptr[0]; // not used here
        std::cout << "push on elink=" << (unsigned) elink_id << " n_bytes=" << (unsigned) n_bytes << "\n";
        #endif

        // 2 is the netio header -- it should not be there
        // 1 is the flat byte
        auto rawData_ptr = q.allocate_n(n_bytes+1); // +1 flat size byte
        // TODO the user has to set it manually:
        rawData_ptr[0] = n_bytes;

        #if MEMCPY > 0
        
        memcpy(&rawData_ptr[1], raw_a_packet, n_bytes*sizeof(uint8_t));

        #else
        memcpy(&rawData_ptr[1], &raw_data_ptr[2], n_bytes*sizeof(uint8_t));
        #endif

        #else

        uint8_t n_bytes = 2 + MAX_ABCs * MAX_CLUSTERS * 2 + 2; // not randomized

        auto rawData_ptr = q.allocate_n(n_bytes+1); // +1 flat size byte
        rawData_ptr[0] = n_bytes;
        auto n_bytes_filled = fill_generated_data(&rawData_ptr[1], myFalse, myFalse, MAX_CLUSTERS, MAX_ABCs, myFalse);
        #if debug_logging > 0
        std::cout << "push directly to the queue n_bytes_filled=" << n_bytes_filled << "\n";
        #endif
        #endif

        q.allocate_store();
        rawData_ptr += n_bytes+1;
      }
    }

    t_consumer.join();
    auto stop = std::chrono::steady_clock::now();

    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count() << " ms" << std::endl;

    std::cout << n_repeat * n_raw_packets * 1000000 /
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
