/*
 *  Rascal Packet Filter, Mainly For OpenWrt
 *  Copyright (C) Cucumber Tony Limited
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.

 *  With thanks to the follow people for helping us on our way:
 *  https://github.com/wertarbyte/blighthouse
 *  https://github.com/Joel-Schofield/RSSI-Sniffer
 *  https://github.com/aircrack-ng/aircrack-ng.git
 *  http://www.tcpdump.org/sniffex.c
 *  https://edux.fit.cvut.cz/oppa/MI-SIB/cviceni/mi-sib-cviceni6.pdf
 *  https://github.com/Otacon22/wifiexperiments
 */

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <syslog.h>
#include <pcap/pcap.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <net/ethernet.h>
/* #include "shared.h" */
#include "radiotap.h"
#include "radiotap_iter.h"
#include <arpa/inet.h>
#include <json-c/json.h>
#include <time.h>
#include <curl/curl.h>

/* #define MAC_CACHE_LEN 30 */
#define MESSAGE_BUFF_LEN 2000 /* 18 LEN OF MAC * 20, MAX CACHE */

// Only for the ethernet tests //

#define SIZE_ETHERNET     14
#define SNAP_LEN          1518
#define ETHER_ADDR_LEN6   6
#define SYSLOG_NAME       "Rascal"

struct sniff_ethernet {
  u_char  ether_dhost[ETHER_ADDR_LEN];    /*  destination host address */
  u_char  ether_shost[ETHER_ADDR_LEN];    /*  source host address */
  u_short ether_type;                     /*  IP? ARP? RARP? etc */
};

struct sniff_ip {
  u_char  ip_vhl;                 /*  version << 4 | header length >> 2 */
  struct  in_addr ip_src,ip_dst;  /*  source and dest address */
};

#define IP_HL(ip)         (((ip)->ip_vhl) & 0x0f)
#define IP_V(ip)          (((ip)->ip_vhl) >> 4)

/*  TCP header */
typedef u_int tcp_seq;

struct sniff_tcp {
  u_short th_sport;               /*  source port */
  u_short th_dport;               /*  destination port */
  tcp_seq th_seq;                 /*  sequence number */
  tcp_seq th_ack;                 /*  acknowledgement number */
  u_char  th_offx2;               /*  data offset, rsvd */
#define TH_OFF(th)      (((th)->th_offx2 & 0xf0) >> 4)
  u_char  th_flags;
#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10
#define TH_URG  0x20
#define TH_ECE  0x40
#define TH_CWR  0x80
#define TH_FLAGS        (TH_FIN|TH_SYN|TH_RST|TH_ACK|TH_URG|TH_ECE|TH_CWR)
  u_short th_win;                 /*  window */
  u_short th_sum;                 /*  checksum */
  u_short th_urp;                 /*  urgent pointer */
};

void ethernet_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet);
void send_data(json_object *data);

// End only for the ethernet tests //

static uint8_t insecure = 0;
static uint8_t verbose = 0;
int mac_array = 30; // Number of macs
int timer = 60; // About 60 seconds still to figure the counter.
static uint8_t deltaforce = 0;
char *config_file = NULL;
char post_url[255];
char if_name[10];
char ap_mac[19];
double lng;
double lat;
/* clock_t c0 = 0; */
time_t start = 0;

static const struct radiotap_align_size align_size_000000_00[] = {
  [0] = { .align = 1, .size = 4, },
  [52] = { .align = 1, .size = 4, },
};

typedef struct {
  u_int8_t        it_version;
  u_int8_t        it_pad;
  u_int16_t       it_len;
  u_int32_t       it_present;

  u_int32_t       pad;
  u_int8_t        flags;
  u_int8_t        rate;
  int8_t          ant_sig;
  int8_t          lock_quality;
  u_int8_t        ant;

} __attribute__((__packed__)) ieee80211_radiotap;

typedef struct {
  unsigned short                  fc;
  unsigned short                  durid;
  u_char  a1[6];
  u_char  a2[6];
  u_char  a3[6];
  unsigned short                  seq;
  u_char  a4[6];
} __attribute__((__packed__)) dot11_header;

void print_mac(FILE * stream,u_char * mac);
void format_mac(u_char * mac, char * f);
void updateRssi(int8_t new_rssi, int8_t prev_rssi, char * key, json_object * json, time_t t0, time_t last_seen);
void updateDelta(int8_t new_rssi, int8_t prev_rssi, json_object * jsonObj);

int array_contains(char * array, char * ip );

struct json_object *obj1, *obj2, *array, *tmp1, *tmp2;

void pcap_callback(u_char *args, const struct pcap_pkthdr *header, const u_char *packet) {

  static int count = 1;
  int diff;
  if (start == 0) {
    start = time(0);
  }

  time_t t0 = time(0);
  int err, i, arraylen, radiotap_header_len;
  int8_t rssi;

  if ( json_object_get_type(array) != json_type_array) {
    if (verbose)
      printf("type of json= %d\n", json_object_get_type(array) == json_type_array);
    array = json_object_new_array();
  };

  char client_mac[18];
  char buf[MESSAGE_BUFF_LEN];

  struct ieee80211_radiotap_iterator iter;

  err = ieee80211_radiotap_iterator_init(&iter, (void*)packet, header->caplen, NULL);
  if (err > 0) {
  }

  radiotap_header_len = iter._max_length;

  while (!(err = ieee80211_radiotap_iterator_next(&iter))) {
    if (iter.this_arg_index == IEEE80211_RADIOTAP_DBM_ANTSIGNAL) {
      rssi = (int8_t)iter.this_arg[0];
    }
    /* if (iter.this_arg_index == IEEE80211_RADIOTAP_DBM_ANTNOISE) { */
    /*   noise = (int8_t)iter.this_arg[0]; */
    /* } */
  };

  if (header->len >= 24 && verbose) {
    u_int8_t hlen;
    hlen = packet[2]+(packet[3]<<8); //Usually 18 or 13 in some cases
    switch (packet[hlen]) {
      case 0x40:
        printf("Probe request\n");
        break;
      case 0x50:
        printf("Probe response\n");
        break;
    }
  };

  dot11_header * dot_head = (dot11_header*) (packet + radiotap_header_len * sizeof(char) );

  format_mac(dot_head->a2, client_mac);

  if (!array_contains(buf, client_mac)) {

    if (verbose)
      printf("Adding this mac: %s to buffer\n", client_mac);
    strcat(buf, client_mac);

    obj2 = json_object_new_object();
    json_object *jclient_mac = json_object_new_string(client_mac);
    json_object *timestamp = json_object_new_int(t0);
    json_object *jrssi = json_object_new_int(rssi);
    json_object_object_add(obj2,"client_mac", jclient_mac);
    json_object_object_add(obj2,"first_seen", timestamp);
    json_object_object_add(obj2,"rssi", jrssi);
    json_object_object_add(obj2,"last_seen", 0);
    json_object_array_add(array,obj2);

  } else {

    if (verbose)
      printf("Updating this mac: %s \n", client_mac);

    arraylen = json_object_array_length(array);
    for (i = 0; i < arraylen; i++) {
      tmp1 = json_object_array_get_idx(array, i);
      json_object_object_get_ex(tmp1, "client_mac", &tmp2);

      /* if ( strcmp(json_object_get_string(tmp2), client_mac) == 0 ) { */
      json_object_object_foreach(tmp1, key, val) {
        time_t last_seen;
        if (strcmp(key, "last_seen") == 0) {
          json_object_object_add(tmp1, key, json_object_new_int(t0));
          last_seen = json_object_get_int(val);
        }
        if ( deltaforce ) {
          if (strcmp(key, "rssi") == 0) {
            int8_t prev_rssi;
            prev_rssi =  json_object_get_int(val);
            updateRssi(rssi, prev_rssi, key, tmp1, t0, last_seen);
          }
        }
      }
      /* } */
    }
  }

  count++;
  diff = (t0 - start);

  if (verbose) {
    printf("Packet number: %d\n", count);
    printf("Time difference is: %d\n", diff);
  }

  if (verbose)
    printf("macs: %d, timer: %d\n", mac_array, timer);

  if ((arraylen >= mac_array && diff > timer) || (arraylen > 0 && diff >= 60)) {
    memset(buf, 0, sizeof buf);
    if (verbose)
      printf ("The json object created: %s\n",json_object_to_json_string(array));
    send_data(array);
    json_object_put(array);
    start = time(0);
    count = 1;
  };

  return;
}

void updateRssi( int8_t new_rssi, int8_t prev_rssi, char * key, json_object * jsonObj, time_t t0, time_t last_seen ) {
  if ( new_rssi != 0 && ( abs(new_rssi) < abs(prev_rssi)) ) {
    json_object_object_add(jsonObj, key, json_object_new_int(new_rssi));
  }
  if ( ( t0 - last_seen ) > 5 ) {
    updateDelta(new_rssi, prev_rssi, jsonObj);
  }
}

void updateDelta(int8_t new_rssi, int8_t prev_rssi, json_object * jsonObj) {
  int8_t delta;
  delta = ( prev_rssi - new_rssi );
  json_object_object_add(jsonObj, "delta", json_object_new_int(delta));
  if(verbose)
  {
    printf("Difference is %hhd\n");
  }
}

void format_mac(u_char * mac, char * f) {
  sprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void print_mac(FILE * stream,u_char * mac) {
  for (int i=0; i < 6; i++) {
    fprintf(stream, "%.2X", mac[i]);
  }
}

int array_contains(char *array, char *data ) {
  strstr(array, data) != NULL;
}


void ethernet_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet)
{

  /* static int count = 1; */
  /* time_t t0 = time(0); */
  /* struct json_object *obj1, *obj2, *array, *tmp1, *tmp2; */

  /* char *val_type_str, *str; */
  /* int val_type, i; */
  /* val_type = json_object_get_type(array); */

  /* switch (val_type) { */
  /*   case json_type_array: */
  /*     val_type_str = "val is an array"; */
  /*     break; */
  /*   default: */
  /*     array = json_object_new_array(); */
  /* } */

  /* obj1 = json_object_new_object(); */

  /* const struct sniff_ethernet *ethernet;  /1* The ethernet header [1] *1/ */
  /* const struct sniff_ip *ip;              /1* The IP header *1/ */
  /* const struct sniff_tcp *tcp;            /1* The TCP header *1/ */
  /* const char *payload;                    /1* Packet payload *1/ */

  /* int size_ip; */
  /* int size_tcp; */
  /* int size_payload; */
  /* char *src_ip; */
  /* char *dst_ip; */

  /* /1* printf("\nPacket number %d:\n", count); *1/ */
  /* count++; */

  /* ethernet = (struct sniff_ethernet*)(packet); */

  /* ip = (struct sniff_ip*)(packet + SIZE_ETHERNET); */
  /* size_ip = IP_HL(ip)*4; */
  /* if (size_ip < 20) { */
  /*   printf("   * Invalid IP header length: %u bytes\n", size_ip); */
  /*   return; */
  /* } */

  /* char buf[MESSAGE_BUFF_LEN]; */

  /* src_ip = inet_ntoa(ip->ip_src); */
  /* dst_ip = inet_ntoa(ip->ip_dst); */

  /* int arraylen; */

  /* if (!array_contains(buf, src_ip)) { */

  /*   obj2 = json_object_new_object(); */
  /*   sprintf(buf, src_ip); */
  /*   json_object *jsrc = json_object_new_string(dst_ip); */
  /*   json_object *timestamp = json_object_new_int(t0); */
  /*   json_object_object_add(obj2,"ip", jsrc); */
  /*   json_object_object_add(obj2,"first_seen", timestamp); */
  /*   json_object_object_add(obj2,"last_seen", 0); */
  /*   json_object_array_add(array,obj2); */

  /* } else { */

  /*   arraylen = json_object_array_length(array); */
  /*   for (i = 0; i < arraylen; i++) { */
  /*     tmp1 = json_object_array_get_idx(array, i); */
  /*     json_object_object_get_ex(tmp1, "ip", &tmp2); */

  /*     int result = strcmp(json_object_get_string(tmp2), dst_ip); */

  /*     if ( result == 0 ) { */

  /*       json_object_object_foreach(tmp1, key, val) { */
  /*         if (strcmp(key, "last_seen") != 0) */
  /*           continue; */
  /*         json_object_object_add(tmp1, key, json_object_new_int(t0)); */
  /*         /1* break; *1/ */
  /*       } */
  /*       break; */
  /*     } */
  /*   } */

  /* } */

  /* if (arraylen >= 10 || (arraylen > 0 && count >= 1000)) { */
  /*   send_data(array); */
  /*   json_object_put(array); */
  /*   count = 1; */
  /* }; */

  return;
}

void send_data(json_object *array) {

  CURL *curl;
  CURLcode res;

  struct curl_slist *headers = NULL;

  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "Content-Type: application/json");

  json_object *obj1 = json_object_new_object();
  json_object *jvs = json_object_new_string("1");
  json_object *japmac = json_object_new_string(ap_mac);
  json_object *jlat = json_object_new_double(lat);
  json_object *jlng = json_object_new_double(lng);

  json_object_object_add(obj1,"v", jvs);
  json_object_object_add(obj1,"ap_mac", japmac);
  json_object_object_add(obj1,"data", array);
  json_object_object_add(obj1,"lat", jlat);
  json_object_object_add(obj1,"lng", jlng);

  curl = curl_easy_init();

  if(curl) {

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, post_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Rascal Bot");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_object_to_json_string(obj1));

    if (insecure) {
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);
    }

    if (verbose) {
      printf ("Sending this: %s\n",json_object_to_json_string(obj1));
    }

    openlog(SYSLOG_NAME, LOG_PID|LOG_CONS, LOG_USER);
    res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
      printf("There was a problem sending to %s\n", post_url);
      syslog (LOG_INFO, "couldn't send JSON.");
    } else {
      syslog (LOG_INFO, "sent JSON to %s", post_url);
    }
    closelog ();
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    json_object_put(obj1);
  }

  curl_global_cleanup();

}

// Anyone else recognise this ?! //
char * read_json_file(char *file)
{
  FILE *infile;
  char *buffer;
  long numbytes;
  infile = fopen(file,"r+");
  if(infile == NULL)
    return "DNE";

  fseek(infile,0L,SEEK_END);
  numbytes = ftell(infile);

  fseek(infile,0L,SEEK_SET);
  buffer = (char*)calloc(numbytes,sizeof(char));

  if(buffer == NULL)
  {
    return NULL;
  }
  fread(buffer,sizeof(char),numbytes,infile);
  fclose(infile);
  return buffer;
}

int readconfig() {

  config_file = "/etc/config.json";

  /* if (config_file == NULL) { */
  /*   config_file = "/etc/config.json"; */
  /* }; */

  char * fp = read_json_file(config_file);

  if ( fp != NULL )
  {

    enum json_type type;

    json_object * jobj = json_tokener_parse(fp);

    if (!is_error(jobj))
    {

      json_object_object_foreach(jobj, key, val0) {

        type = json_object_get_type(val0);

        switch (type) {
          case json_type_double:
            if (strcmp(key,"lat") == 0) {
              lat = json_object_get_double(val0);
            }
            if (strcmp(key,"lng") == 0) {
              lng = json_object_get_double(val0);
            }
            break;
          case json_type_string:
            if (strcmp(key,"rs_url") == 0) {
              strcpy(post_url, json_object_get_string(val0));
            }
            if (strcmp(key,"mac") == 0) {
              strcpy(ap_mac, json_object_get_string(val0));
            }
            if (strcmp(key,"rs_iface") == 0) {
              strcpy(if_name, json_object_get_string(val0));
            }
            /* if (strcmp(key,"location_id") == 0) { */
            /*   strcpy(id, json_object_get_string(val0)); */
            /* } */
            /* if (strcmp(key,"rs_token") == 0) { */
            /*   /1* token = "123"; *1/ */
            /*   /1* token = json_object_get_string(val0); *1/ */
            /*   strcpy(token, json_object_get_string(val0)); */
            /* } */
            break;
        }
      }
    } else {
      printf("Could not read json\n");
      return 0;
    }

    /* if (verbose) */
    /* printf("lat: %d, lng: %d, id: %s\n", lat, lng, id); */

    json_object_put(jobj);

  };
  return 1;
}

void intHandle(int dummy){

  openlog(SYSLOG_NAME, LOG_PID|LOG_CONS, LOG_USER);
  syslog (LOG_INFO, "exiting listener...");
  closelog ();
  exit(1);

};

/* void set_url() { */
/*     sprintf(full_url, "%s?id=%s&token=%s", post_url, id, token); */
/*     printf("orig: %s, token: %s, url: %s\n", post_url, token, full_url); */
/* } */

int main(int argc, char *argv[]) {

  signal(SIGINT, intHandle);
  curl_global_init( CURL_GLOBAL_ALL );

  char pcap_errbuf[PCAP_ERRBUF_SIZE];
  struct bpf_program fp;
  char filter_exp[] = "ip";
  bpf_u_int32 net;
  pcap_errbuf[0] = '\0';

  int  c;
  opterr = 0;

  while ((c = getopt(argc, argv, "i:m:c:t:a:t:vkd")) != -1) {
    switch(c) {
      case 'i':
        strcpy(if_name, optarg);
        break;
      case 'm':
        strcpy(ap_mac, optarg);
        break;
      case 'k':
        insecure = 1;
        break;
      case 'v':
        verbose = 1;
        break;
      case 'c':
        config_file = optarg;
        break;
      case 't':
        timer = atoi(optarg);
        break;
      case 'd':
        deltaforce = 1;
        break;
      case 'a':
        mac_array = atoi(optarg);
        break;
      default:
        abort();
    }
  }

  readconfig();

  if ( if_name == NULL ) {
    strcpy(if_name, "eth0");
  }

  if (verbose)
    printf("Listen on interface %s\n", if_name);

  openlog(SYSLOG_NAME, LOG_PID|LOG_CONS, LOG_USER);
  syslog (LOG_INFO, "listener has been started on %s ", if_name);
  closelog ();

  pcap_t *pcap = pcap_open_live(if_name, 1024, 0, 1, pcap_errbuf);
  pcap_set_promisc(pcap, 1);

  if (!pcap) {
    printf("%s\n", pcap_errbuf);
    exit(1);
  }

  /* set_url(); */

  int link_layer_type = pcap_datalink(pcap);

  if (link_layer_type != DLT_IEEE802_11_RADIO) {
    fprintf(stderr, "Not using the Wi-Fi interface, are you testing something?\n");
    if (pcap_compile(pcap, &fp, filter_exp, 0, net) == -1) {
      fprintf(stderr, "Couldn't parse filter %s: %s\n", filter_exp, pcap_geterr(pcap));
      exit(EXIT_FAILURE);
    }
    if (pcap_setfilter(pcap, &fp) == -1) {
      fprintf(stderr, "Couldn't install filter %s: %s\n",
          filter_exp, pcap_geterr(pcap));
      exit(EXIT_FAILURE);
    }
    pcap_loop(pcap, -1, ethernet_packet, NULL);

  } else {

    struct bpf_program filter_probe_req;
    struct bpf_program filter_probe_resp;
    pcap_compile(pcap, &filter_probe_req, "type mgt subtype probe-req", 1, PCAP_NETMASK_UNKNOWN);
    pcap_setfilter(pcap, &filter_probe_req);

    /* pcap_compile(pcap, &filter_probe_resp, "type mgt subtype probe-resp", 1, PCAP_NETMASK_UNKNOWN); */
    /* pcap_setfilter(pcap, &filter_probe_resp); */

    pcap_loop(pcap, -1, pcap_callback, NULL);
  }

  printf("Done");
  pcap_close(pcap);

  return 0;

}
