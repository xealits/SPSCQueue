#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <string.h> // memset

//const static uint8_t TYP_LP = 2;
#define TYP_LP 2

typedef enum { myFalse, myTrue } myBool;

//extern int debug_logging;

#ifndef debug_logging
  #define debug_logging 0
#endif

//typedef union {
//    struct {
//        //uint8_t abc_id: 5;
//        //uint8_t x: 4;
//        //uint8_t row: 4;
//        //uint8_t strips_pattern: 3;
//        uint8_t abc_id : 8;
//        uint8_t x : 8;
//        uint8_t row : 8;
//        uint8_t strips_pattern : 8;
//    } fields;
//    uint32_t bits;
//} some_struct_t;

//typedef union {
//	struct {
//		uint8_t strips_pattern :8; // Or do we want separate hits from 1 cluster?
//		uint8_t row :8; // ?
//		uint8_t x :8;
//		uint8_t abc_id :8;
//	} fields;
//	uint32_t bits;
//} FrontEndHit;

// or just:
typedef union {
	struct __attribute__((packed)) {
		uint8_t strips_pattern :3; // Or do we want separate hits from 1 cluster?
		uint8_t row :7; // ?
		uint8_t x :1;
		uint8_t abc_id :4;
	} fields;
	uint16_t bits;
} FrontEndHit;

struct FrontEndData {
	uint8_t flag;
	uint8_t l0id;
	uint8_t bcid;
	uint32_t n_hits;
	FrontEndHit * fe_hits;
};

/// @brief  flat FE data format
/// the first item = how many hits in the packet
/// then follows the data header, with l0id etc
/// and the hits
typedef union {
	struct __attribute__((packed)) {
		uint8_t strips_pattern;
		uint8_t address; // address, do not split the row and x
		uint8_t abc_id;
		uint8_t reserved;
	} hit;

	struct __attribute__((packed)) {
		uint8_t flag; // 1 full byte
		uint8_t l0id;
		uint8_t bcid;
		uint8_t reserved;
	} header;

	uint32_t n_hits;
} FrontEndData_flat;

typedef struct {
	uint8_t l0id;
	uint8_t reserved; // whatever flags & errors?
} FrontEndDataStats;

/*
 * a packet:
 * netio header | star data
 * - netio header = bytes([elink, int(len(star_data)/4)]) -- the packet length in 32bit words?
 *   shouldn't it be in bytes?
 * - star data = 
 *   example: Star process_data 0 -->  20  0  8  4 10  4 18  4 20  4 28  4 30  4 38  0 40  4 48  4 50  4 6f edPacket type TYP_LP, BCID 0 (0), L0ID 0, nClusters a
 *   a_packet = b'\x20\x00 \x08\x04\x10\x04\x18\x04\x20\x04\x28\x04\x30\x04\x38\x00\x40\x04\x48\x04\x50\x04 \x6f\xed'
 *   header, 2 bytes:
 *   4b = TYP = 2 for physics
 *   1b = Flag
 *   7b = L0ID
 *   4b = BCID (3b BCID and 1b parity)
 *   then variable number of clusters (2 bytes each, no separators):
 *   0 4b input channel | 11b cluster info without the last claster flag = 8b address | 3b next strips
 *   (i.e. from 1b last cluster flag 8b cluster address 3b next strips pattern)
 *   event with no hits = "no cluster byte" = 0x03FE ? -> without the last cluster 1b = 0x3FE ?
 *   then end of packet 2 bytes:
 *   0x6fed
 *
 * so, let's generate
 * 1) always data from each ABC,
 * 2) up to 4 clusters per ABC?
 * 3) with 0-3 next strips?
 * 
 */

/// the range is inclusive [lower-upper]
int random_int(int lower, int upper) {
	int num = (rand() % (upper - lower + 1)) + lower;
	return num;
}

/// returns the N bytes that it filled
/// the user must make sure it fits in the pointer
///
/// max size is when each ABC outputs 4 clusters + 2 bytes header and 2 bytes footer
/// max_n_abcs * max_n_clusters * 2 + (2) + (2)
/// and netio header = 2 bytes
unsigned fill_generated_data(uint8_t* raw_data, myBool randomize_packets, myBool big_endianness, unsigned max_n_abcs, unsigned max_n_clusters, myBool with_netio_header) {
	uint8_t elink_id = 0;
	unsigned n_bytes_star_data = 0; // to be calculated

	unsigned n_bytes_netio_header=0;
	if (with_netio_header) {
	  n_bytes_netio_header = 2;
	  raw_data[0] = elink_id;
	  //raw_data[1] = n_bytes_star_data; // TODO don't forget to fill
	}

	// star data
	unsigned cur_i = 0;
	if (with_netio_header) cur_i=2;
	uint8_t flag = 0;
	uint8_t l0id = 0xC;
	uint8_t bcid = 0x3;

	if (big_endianness) {
		raw_data[cur_i++] = ((TYP_LP & 0xf) << 4) | ((flag & 0x1) << 3) | ((l0id & 0x70) >> 4);
		raw_data[cur_i++] = ((l0id & 0xf) << 4) | (bcid & 0xf);
	} else {
		raw_data[cur_i++] = ((l0id & 0xf) << 4) | (bcid & 0xf);
		raw_data[cur_i++] = ((TYP_LP & 0xf) << 4) | ((flag & 0x1) << 3) | ((l0id & 0x70) >> 4);
	}
	n_bytes_star_data += 2;

	// cluster bytes
	for (uint8_t abc_id=1; abc_id<=max_n_abcs; abc_id++) {
		unsigned n_clusters_from_cur_ABC = max_n_clusters;
		if (randomize_packets) {
			n_clusters_from_cur_ABC = random_int(1, max_n_clusters);
		}

		for (unsigned n_cluster = 0; n_cluster<n_clusters_from_cur_ABC; n_cluster++) {
			uint8_t strips_address = 127;
			uint8_t strips_pattern = 3;
			if (randomize_packets) {
				strips_address = random_int(0, 255); // the range is inclusive
				strips_pattern = random_int(0, 7);
			}

			// now the cluster bytes:
			uint8_t cl1 = ((abc_id&0xf)<<3) | ((strips_address>>5) & 0x7);
			uint8_t cl2 = ((strips_address & 0x1f)<<3) | (strips_pattern & 0x7);

			if (big_endianness) {
				raw_data[cur_i++] = cl1;
				raw_data[cur_i++] = cl2;
			} else {
				raw_data[cur_i++] = cl2;
				raw_data[cur_i++] = cl1;
			}
			n_bytes_star_data += 2;

			if (debug_logging>1) printf("fill_generated_data: %3d %3d, cluster bytes - %2x %2x\n", strips_address, strips_pattern, cl1, cl2);
		}
	}

	// and the end of packet
	raw_data[cur_i++]  = 0x6f;
	raw_data[cur_i++]  = 0xed;
	n_bytes_star_data += 2;

	if (n_bytes_star_data > 255) printf("fill_generated_data: ERROR n_bytes_star_data > 255: %d\n", n_bytes_star_data);
	
	if (with_netio_header) // fill the netio payload byte
	  raw_data[1] = n_bytes_star_data & 0xFF;

	if (debug_logging>1) {
      printf("fill_generated_data raw bytes:");
	  for (unsigned ibyte=0; ibyte<n_bytes_star_data + n_bytes_netio_header; ibyte++) {
        printf(" %x", raw_data[ibyte]);
	  }
	}

	return n_bytes_star_data + n_bytes_netio_header;
}

void print_FrontEndData(struct FrontEndData* fe_data) {
	uint32_t n_hits = fe_data->n_hits;
	FrontEndHit * fe_hits = fe_data->fe_hits;
	printf("print_FrontEndData: %d %x n_hits=%d |", fe_data->l0id, fe_data->bcid, n_hits);
	for (int i=0; i<n_hits; i++) {
		printf(" %d:%d-%d-%d", fe_hits[i].fields.abc_id, fe_hits[i].fields.row, fe_hits[i].fields.x, fe_hits[i].fields.strips_pattern);
	}
	printf("\n");
}

extern inline void parse_data(uint8_t* raw_data, uint8_t n_data_bytes, struct FrontEndData* out_data) {
	// basic checks
	// header 2 bytes
	/* endianness
	uint8_t typ = (raw_data[0] & 0xf0) >> 4;
	if (typ != TYP_LP) {
		printf("parse_data: ERROR header is not TYP_LP %d\n", raw_data[0]);
		return;
	}

	uint8_t flag = (raw_data[0] & 0x8) >> 3;
	uint8_t l0id = ((raw_data[0] & 0x7) << 4) | ((raw_data[1] & 0xf0) >> 4);
	uint8_t bcid = raw_data[1] & 0xf;
	*/

	if (n_data_bytes<6) {
		printf("parse_data: ERROR the packet is empty or contains 1 byte for clusters: %d (-2-2)\n", n_data_bytes);
		return;
	}

	//
	uint16_t header = *(uint16_t*) (&raw_data[0]);
	uint16_t footer = *(uint16_t*) (&raw_data[n_data_bytes-2]);

	uint8_t typ = (header & 0xf000) >> (4+8);
	//uint8_t typ = (header & 0xf0) >> (4);
	if (typ != TYP_LP) {
		printf("parse_data: ERROR header is not TYP_LP %d\n", raw_data[0]);
		return;
	}

	uint8_t flag = (header & 0x800) >> (3+8);
	uint8_t l0id = (header >> 4) & 0x7f;
	uint8_t bcid = header & 0xf;

	// packet end
	//if (raw_data[n_data_bytes-2] != 0x6f || raw_data[n_data_bytes-1] != 0xed)
	if (footer != 0xed6f)
	{
		printf("parse_data: ERROR footer is not 0x6f ed : 0x%x 0x%x\n", raw_data[n_data_bytes-2], raw_data[n_data_bytes-1]);
		return;
	}

	uint8_t n_cluster_bytes = n_data_bytes - 2 - 2;
	// must be divisible by 2
	if (n_cluster_bytes & 0x1) {
		printf("parse_data: ERROR odd number of cluster bytes: %d\n", n_cluster_bytes);
		return;
	}
	uint8_t n_clusters = n_cluster_bytes >> 1;

	uint16_t* cl_array = ((uint16_t*) raw_data) + 1;
	//uint16_t cl = *(uint16_t*) (&raw_data[2 + n_cluster*2 + 0]);

	out_data->flag = flag;
	out_data->l0id = l0id;
	out_data->bcid = bcid;
	out_data->n_hits = n_clusters;

	FrontEndHit * fe_hits = out_data->fe_hits;
	// clusters
	// here 1 cluster = 1 hit, the strips pattern is copied
	//for (uint8_t n_cluster=0; n_cluster<n_clusters; n_cluster++)
	for (uint8_t n_cluster=0; n_cluster<10; n_cluster++)
	{ // assume 10 abcs with 1 cluster each
		/*
		//
		uint8_t byte0 = raw_data[2 + n_cluster*2 + 0];
		uint8_t byte1 = raw_data[2 + n_cluster*2 + 1];

		//uint8_t cl1 = ((abc_id & 0xf)<<3) | ((strips_address>>5) & 0x7);
		//uint8_t cl2 = ((strips_address & 0x1f)<<3) | (strips_pattern & 0x7);

		uint8_t abc_id  = (byte0>>3) & 0xf;
		uint8_t address = ((byte0 & 0x7) << 5) | ((byte1 >> 3) & 0x1f);
		uint8_t strips_pattern = byte1 & 0x7;
		*/


		// row and x

		//// direct
		//uint16_t cl = cl_array[n_cluster]; // *(uint16_t*) (&raw_data[2 + n_cluster*2 + 0]);
		//uint8_t abc_id  = cl >> 11;
		//uint8_t address = (cl >> 3) & 0xff;
		////uint8_t row = (cl >> 10) & 0x1;
		////uint8_t x   = (cl >> 3)  & 0x7f;
		//uint8_t strips_pattern  = cl & 0x7;

		//fe_hits[n_cluster].abc_id = abc_id;
		////fe_hits[n_cluster].row    = row; // (address & 0x80) >> 7;
		////fe_hits[n_cluster].x      = x; // address & 0x7f;
		//fe_hits[n_cluster].row    = (address & 0x80) >> 7;
		//fe_hits[n_cluster].x      =  address & 0x7f;
		//fe_hits[n_cluster].strips_pattern = strips_pattern;

		//// packed
		//uint32_t cl = cl_array[n_cluster]; // *(uint16_t*) (&raw_data[2 + n_cluster*2 + 0]);
		//uint32_t abc_id  = (cl & (0xf << 11)) << (24 - 11);
		//uint32_t address = (cl >> 3) & 0xff;
		//uint32_t row = (address & (0x1  << 7)) << (16 - 7);
		//uint32_t x   = (address & (0x7f))      << (8);
		//uint32_t strips_pattern  = cl & 0x7;
		//fe_hits[n_cluster].bits = abc_id | row | x | strips_pattern;
		//printf("parse_data: cl %x -> %x %x %x\n", cl, abc_id, address, strips_pattern);

		// or just bit fields:
		fe_hits[n_cluster].bits = cl_array[n_cluster];
		//printf("parse_data: cl %x -> %x %x %x %x\n", cl_array[n_cluster], fe_hits[n_cluster].fields.abc_id, fe_hits[n_cluster].fields.row, fe_hits[n_cluster].fields.x, fe_hits[n_cluster].fields.strips_pattern);
	}
}

void parse_data_2(uint8_t* raw_data, uint8_t n_data_bytes, uint8_t n_data_bytes_2, struct FrontEndData* out_data) {
	//parse_data(raw_data,                n_data_bytes,   out_data);
	//parse_data(raw_data+n_data_bytes+2, n_data_bytes_2, out_data+1);

	// basic checks
	uint8_t typ = (raw_data[0] & 0xf0) >> 4;
	if (typ != TYP_LP) {
		printf("parse_data: ERROR header is not TYP_LP %d\n", raw_data[0]);
		return;
	}

	uint8_t flag = (raw_data[0] & 0x8) >> 3;
	uint8_t l0id = ((raw_data[0] & 0x7) << 4) | ((raw_data[1] & 0xf0) >> 4);
	uint8_t bcid = raw_data[1] & 0xf;

	// packet end
	if (raw_data[n_data_bytes-2] != 0x6f || raw_data[n_data_bytes-1] != 0xed) {
		printf("parse_data: ERROR footer is not 0x6f ed : 0x%x 0x%x\n", raw_data[n_data_bytes-2], raw_data[n_data_bytes-1]);
		return;
	}

	uint8_t n_cluster_bytes = n_data_bytes - 2 - 2;
	// must be divisible by 2
	if (n_cluster_bytes & 0x1) {
		printf("parse_data: ERROR odd number of cluster bytes: %d\n", n_cluster_bytes);
		return;
	}
	uint8_t n_clusters = n_cluster_bytes >> 1;

	out_data->flag = flag;
	out_data->l0id = l0id;
	out_data->bcid = bcid;
	out_data->n_hits = n_clusters;

	FrontEndHit * fe_hits = out_data->fe_hits;
	// clusters
	// here 1 cluster = 1 hit, the strips pattern is copied
	for (uint8_t n_cluster=0; n_cluster<n_clusters; n_cluster++) {
		//
		uint8_t byte0 = raw_data[2 + n_cluster*2 + 0];
		uint8_t byte1 = raw_data[2 + n_cluster*2 + 1];

		//uint8_t cl1 = ((abc_id & 0xf)<<3) | ((strips_address>>5) & 0x7);
		//uint8_t cl2 = ((strips_address & 0x1f)<<3) | (strips_pattern & 0x7);

		uint8_t abc_id  = (byte0>>3) & 0xf;
		uint8_t address = ((byte0 & 0x7) << 5) | ((byte1 >> 3) & 0x1f);
		uint8_t strips_pattern = byte1 & 0x7;

		// row and x
		fe_hits[n_cluster].fields.abc_id = abc_id;
		fe_hits[n_cluster].fields.row    = (address & 0x80) >> 7;
		fe_hits[n_cluster].fields.x      = address & 0x7f;
		fe_hits[n_cluster].fields.strips_pattern = strips_pattern;

		//printf("parse_data: abc_id %d \n");
	}

	/* */
	uint8_t* raw_data2 = &raw_data[n_data_bytes+2]; // +2 = netio header

	// basic checks
	uint8_t typ2 = (raw_data2[0] & 0xf0) >> 4;
	if (typ2 != TYP_LP) {
		printf("parse_data: ERROR header is not TYP_LP %d\n", raw_data2[0]);
		return;
	}

	uint8_t flag2 = (raw_data2[0] & 0x8) >> 3;
	uint8_t l0id2 = ((raw_data2[0] & 0x7) << 4) | ((raw_data2[1] & 0xf0) >> 4);
	uint8_t bcid2 = raw_data2[1] & 0xf;

	// packet end
	if (raw_data2[n_data_bytes_2-2] != 0x6f || raw_data2[n_data_bytes_2-1] != 0xed) {
		printf("parse_data: ERROR footer is not 0x6f ed : 0x%x 0x%x\n", raw_data2[n_data_bytes_2-2], raw_data2[n_data_bytes_2-1]);
		return;
	}

	uint8_t n_cluster_bytes2 = n_data_bytes_2 - 2 - 2;
	// must be divisible by 2
	if (n_cluster_bytes2 & 0x1) {
		printf("parse_data: ERROR odd number of cluster bytes: %d\n", n_cluster_bytes);
		return;
	}
	uint8_t n_clusters2 = n_cluster_bytes2 >> 1;

	struct FrontEndData* out_data2 = out_data+1;
	out_data2->flag = flag2;
	out_data2->l0id = l0id2;
	out_data2->bcid = bcid2;
	out_data2->n_hits = n_clusters2;

	FrontEndHit * fe_hits2 = out_data2->fe_hits;
	// clusters
	// here 1 cluster = 1 hit, the strips pattern is copied
	for (uint8_t n_cluster=0; n_cluster<n_clusters; n_cluster++) {
		//
		uint8_t byte0 = raw_data2[2 + n_cluster*2 + 0];
		uint8_t byte1 = raw_data2[2 + n_cluster*2 + 1];

		//uint8_t cl1 = ((abc_id & 0xf)<<3) | ((strips_address>>5) & 0x7);
		//uint8_t cl2 = ((strips_address & 0x1f)<<3) | (strips_pattern & 0x7);

		uint8_t abc_id  = (byte0>>3) & 0xf;
		uint8_t address = ((byte0 & 0x7) << 5) | ((byte1 >> 3) & 0x1f);
		uint8_t strips_pattern = byte1 & 0x7;

		// row and x
		fe_hits2[n_clusters2].fields.abc_id = abc_id;
		fe_hits2[n_clusters2].fields.row    = (address & 0x80) >> 7;
		fe_hits2[n_clusters2].fields.x      = address & 0x7f;
		fe_hits2[n_clusters2].fields.strips_pattern = strips_pattern;

		//printf("parse_data: abc_id %d \n");
	}


}


