#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#include <rte_ring.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_branch_prediction.h>

#include "lzo/lzowrite.h"
#include "lzo/minilzo.h"
#include "pcap.h"
#include "utils.h"

#include "core_write.h"

#define MIN(a,b) (((a)<(b))?(a):(b))

#define RTE_LOGTYPE_DPDKCAP RTE_LOGTYPE_USER1

/*
 * Change file name
 */
static void fillTemplate(
    char * filename,
    const char * template,
    const int core_id,
    const int file_count,
    const struct timeval * file_start
    ) {
  char str_buf[DPDKCAP_OUTPUT_FILENAME_LENGTH];
  //Change file name
  strncpy(filename, template,
      DPDKCAP_OUTPUT_FILENAME_LENGTH);
  snprintf(str_buf, 50, "%02d", core_id);
  while(str_replace(filename,"\%COREID",str_buf));
  snprintf(str_buf, 50, "%03d", file_count);
  while(str_replace(filename,"\%FCOUNT",str_buf));
  strncpy(str_buf, filename, DPDKCAP_OUTPUT_FILENAME_LENGTH);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  strftime(filename, DPDKCAP_OUTPUT_FILENAME_LENGTH, str_buf,
      localtime(&(file_start->tv_sec)));
#pragma GCC diagnostic pop
}

/*
 * Opens a new lzowrite_buffer and write a new pcap header
 */
static int open_lzo_pcap(
    struct lzowrite_buffer * buffer,
    char * output_file,
    unsigned int snaplen) {
  struct pcap_header pcp;

  //Create new buffer
  if(lzowrite_init(buffer, output_file)) return -1;

  //Write pcap header
  pcap_header_init(&pcp, snaplen);
  lzowrite32(buffer, pcp.magic_number);
  lzowrite16(buffer, pcp.version_major);
  lzowrite16(buffer, pcp.version_minor);
  lzowrite32(buffer, pcp.thiszone);
  lzowrite32(buffer, pcp.sigfigs);
  lzowrite32(buffer, pcp.snaplen);
  lzowrite32(buffer, pcp.network);

  return 0;
}

/*
 * Write the packets form the write ring into a pcap compressed file
 */
int write_core(const struct core_write_config * config) {
  struct lzowrite_buffer write_buffer;
  unsigned char* eth;
  unsigned int packet_length, wire_packet_length;
  int result;
  void* dequeued[DPDKCAP_WRITE_BURST_SIZE];
  struct rte_mbuf* bufptr;
  struct pcap_packet_header header;
  struct timeval tv;

  char file_name[DPDKCAP_OUTPUT_FILENAME_LENGTH];
  unsigned int file_count = 0;
  unsigned int file_size = 0;
  struct timeval file_start;

  gettimeofday(&file_start, NULL);

  //Update filename
  fillTemplate(file_name, config->output_file_template,
      rte_lcore_id(), file_count, &file_start);

  //Init stats
  *(config->stats) = (struct core_write_stats) {
      .core_id=rte_lcore_id(),
      .current_file_packets=0,
      .current_file_bytes=0,
      .current_file_compressed_bytes=0,
      .packets = 0,
      .bytes = 0,
      .compressed_bytes = 0,
  };
  memcpy(config->stats->output_file, file_name,
      DPDKCAP_OUTPUT_FILENAME_LENGTH);

  //Open new lzo file
  if(open_lzo_pcap(&write_buffer, file_name, config->snaplen))
    return -1;

  //Log
  RTE_LOG(INFO, DPDKCAP, "Core %d is writing in file : %s.\n",
      rte_lcore_id(), config->output_file_template);

  for (;;) {
    if (unlikely(*(config->stop_condition))) {
      break;
    }
    //Get packets from the ring
    result = rte_ring_dequeue_burst(config->ring,
        dequeued, DPDKCAP_WRITE_BURST_SIZE);
    if (result == 0) {
      continue;
    }

    //Update stats
    config->stats->packets += result;

    int i;
    bool file_changed;
    for (i = 0; i < result; i++) {
      //Cast to packet
      bufptr = dequeued[i];
      eth = rte_pktmbuf_mtod(bufptr, unsigned char*);
      wire_packet_length = rte_pktmbuf_pkt_len(bufptr);
      //Truncate packet if needed
      packet_length = MIN(config->snaplen,wire_packet_length);

      //Get time
      gettimeofday(&tv, NULL);

      //Create a new file according to limits
      file_changed = 0;
      if(config->rotate_seconds &&
          (uint32_t)(tv.tv_sec-file_start.tv_sec) >= config->rotate_seconds) {
        file_count=0;
        gettimeofday(&file_start, NULL);
        file_changed=1;
      }
      if(config->file_size_limit && file_size >= config->file_size_limit) {
        file_count++;
        file_changed=1;
      }

      //Open new file
      if(file_changed) {
        //Update data
        file_size=0;

        //Change file name
        fillTemplate(file_name, config->output_file_template,
            rte_lcore_id(), file_count, &file_start);

        //Update stats
        config->stats->current_file_packets = 0;
        config->stats->current_file_bytes = 0;
        memcpy(config->stats->output_file, file_name,
            DPDKCAP_OUTPUT_FILENAME_LENGTH);

        //Close pcap file and open new one
        lzowrite_free(&write_buffer);
        if(open_lzo_pcap(&write_buffer, file_name, config->snaplen))
          return -1;
      }

      //Write block header
      header.timestamp = (int32_t) tv.tv_sec;
      header.microseconds = (int32_t) tv.tv_usec;
      header.packet_length = packet_length;
      header.packet_length_wire = wire_packet_length;
      lzowrite(&write_buffer, &header, sizeof(struct pcap_packet_header));

      //Write content
      lzowrite(&write_buffer, eth, sizeof(char) * packet_length);

      //Update file data
      file_size += write_buffer.out_length;

      //Update stats
      config->stats->bytes += packet_length;
      config->stats->compressed_bytes += write_buffer.out_length;
      config->stats->current_file_packets ++;
      config->stats->current_file_bytes += packet_length;
      config->stats->current_file_compressed_bytes = file_size;

      //Free buffer
      rte_pktmbuf_free(bufptr);
    }
  }
  //Close pcap file
  lzowrite_free(&write_buffer);
  RTE_LOG(INFO, DPDKCAP, "Closed writing core %d\n",rte_lcore_id());
  return 0;
}

