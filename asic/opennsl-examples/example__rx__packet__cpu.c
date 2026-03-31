    1 /*********************************************************************
    2  *
    3  * (C) Copyright Broadcom Corporation 2013-2017
    4  *
    5  *  Licensed under the Apache License, Version 2.0 (the "License");
    6  *  you may not use this file except in compliance with the License.
    7  *  You may obtain a copy of the License at
    8  *
    9  *      http://www.apache.org/licenses/LICENSE-2.0
   10  *
   11  *  Unless required by applicable law or agreed to in writing, software
   12  *  distributed under the License is distributed on an "AS IS" BASIS,
   13  *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   14  *  See the License for the specific language governing permissions and
   15  *  limitations under the License.
   16  *
   17  **********************************************************************
   18  * \file     example_rx_packet_cpu.c
   19  *
   20  * \brief    Example code for packet transmission
   21  *
   22  * \details  This example demonstrates the reception of a CPU bound packet.
   23  *
   24  ************************************************************************/
   25 
   26 #include <stdio.h>
   27 #include <stdlib.h>
   28 #include <string.h>
   29 #include <sal/driver.h>
   30 #include <sal/version.h>
   31 #include <opennsl/error.h>
   32 #include <opennsl/vlan.h>
   33 #include <opennsl/port.h>
   34 #include <opennsl/switch.h>
   35 #include <opennsl/pkt.h>
   36 #include <opennsl/rx.h>
   37 #include <examples/util.h>
   38 
   39 #define DEFAULT_UNIT  0
   40 #define DEFAULT_VLAN  1
   41 #define MAX_DIGITS_IN_CHOICE 5
   42 
   43 char example_usage[] =
   44 "Syntax: example_rx_packet_cpu                                         \n\r"
   45 "                                                                      \n\r"
   46 "Paramaters: None.                                                     \n\r"
   47 "                                                                      \n\r"
   48 "Example: The following command is used to demonstrate receiving a     \n\r"
   49 "         packet to CPU.                                               \n\r"
   50 "         example_rx_packet_cpu                                        \n\r"
   51 "                                                                      \n\r"
   52 "Usage Guidelines: None.                                               \n\r";
   53 
   54 
   55 /*****************************************************************/
   68 int example_pkt_start_get (unsigned int unit, unsigned char *pkt,
   69                            int len, unsigned char *user_header,
   70                            unsigned int *offset)
   71 {
   72   unsigned char        pph_type;
   73   unsigned int         pph_offset;
   74   unsigned int         lb_ext_offset = 0;
   75   unsigned char        eei_ext_present;
   76   unsigned char        lrn_ext_present;
   77   unsigned char        fhei_size;
   78 
   79   /* Initialize offset to account for the FTMH base header size */
   80   *offset = 9;
   81 
   82   /* Set to 1 if config property "system_ftmh_load_balancing_ext_mode" is *
   83    * defined. Otherwise, set it to zero */
   84   lb_ext_offset = 1;
   85   *offset += lb_ext_offset;
   86 
   87   /*
   88    * Need to parse the FTMH Base header to determine if Dest System Port
   89    * Ext(16bits) is included. This is bit 3 of the 72bit FTMH base header.
   90    */
   91   if (pkt[8] & 0x08)
   92   {
   93     *offset += 2;
   94   }
   95 
   96   /* Parse the PPH_TYPE in the FTMH header to see how much, *
   97    * if any, PPH to parse */
   98   pph_type = (pkt[5] & 0x06) >> 1;
   99   switch(pph_type)
  100   {
  101     case 0: /* No PPH */
  102       pph_offset = 0;
  103       break;
  104 
  105     case 1: /* PPH Base */
  106       /* PPH Base size = 56bits */
  107       pph_offset = 7;
  108 
  109       /* Is EEI-Extension-Present 24bits */
  110       eei_ext_present = (pkt[*offset+0] & 0x80) >> 7;
  111       if (eei_ext_present)
  112       {
  113         pph_offset += 3;
  114       }
  115 
  116       /* Is Learn-Extension-Present 40bits */
  117       lrn_ext_present = (pkt[*offset+0] & 0x40) >> 6;
  118       if (lrn_ext_present)
  119       {
  120         pph_offset += 5;
  121       }
  122 
  123       /* FHEI-Size? */
  124       fhei_size = (pkt[*offset+0] & 0x30) >> 4;
  125       switch(fhei_size)
  126       {
  127         case 0:
  128           /* No fhei header */
  129           break;
  130         case 1:
  131           /* 3 Byte fhei header */
  132           pph_offset += 3;
  133           break;
  134         case 2:
  135           /* 5 Byte fhei header */
  136           pph_offset += 5;
  137           break;
  138         case 3:
  139           /* 8 Byte fhei header */
  140           pph_offset += 8;
  141           break;
  142       }
  143       break;
  144 
  145     case 2: /* PPH OAM-TS only */
  146       pph_offset = 0;
  147       break;
  148 
  149     case 3: /* PPH Base + PAM-TS */
  150       pph_offset = 0;
  151       break;
  152    }
  153 
  154   *offset += pph_offset;
  155 
  156   return 0;
  157 }
  158 /* __doxy_func_body_end__ */
  159 
  160 /*****************************************************************/
  169 opennsl_rx_t example_rx_packet_receive(int unit, opennsl_pkt_t *pkt,
  170                                        void *cookie)
  171 {
  172   int i;
  173   uint8 *data;
  174   unsigned int offset = 0;
  175   opennsl_pkt_t pkt_l;
  176   unsigned char user_header[8];
  177 
  178   /* print the packet received from driver */
  179   printf("Packet received with pkt->rx_port:%hhu pkt->src_port:%hhu\n",
  180       pkt->rx_port, pkt->src_port);
  181   printf("Packet received with pkt->pkt_len : %d pkt->tot_len : %d cos: %d\n",
  182       pkt->pkt_len, pkt->tot_len, pkt->cos);
  183 
  184   data = pkt->pkt_data->data;
  185 
  186   for(i = 0; i < pkt->tot_len; i++)
  187   {
  188     if ((i % 4) == 0)
  189       printf(" ");
  190     if ((i % 32) == 0)
  191       printf("\n\t");
  192 
  193     printf("%02hhx", data[i]);
  194   }
  195   printf("\n\n");
  196 
  197   /* Remove device specific header from the received packet */
  198   memset (&pkt_l, 0x00, sizeof(pkt_l));
  199   example_pkt_start_get(unit, pkt->pkt_data->data,
  200       pkt->pkt_data->len,
  201       user_header,
  202       &offset);
  203 
  204   memcpy (&pkt_l, pkt, sizeof(pkt_l));
  205   pkt_l.pkt_data->data = &pkt->pkt_data->data[offset];
  206   pkt_l.tot_len        =  pkt->tot_len - offset;
  207   pkt = &pkt_l;
  208 /*  pkt->src_port = pkt->src_port - 12; */
  209 
  210   /* print the packet after removing device specific header */
  211   printf("Packet after stripping the device specific header\n");
  212   data = pkt->pkt_data->data;
  213 
  214   for(i = 0; i < pkt->tot_len; i++)
  215   {
  216     if ((i % 4) == 0)
  217       printf(" ");
  218     if ((i % 32) == 0)
  219       printf("\n\t");
  220 
  221     printf("%02hhx", data[i]);
  222   }
  223 
  224   printf("\n\n");
  225 
  226   return OPENNSL_RX_HANDLED;
  227 }
  228 /* __doxy_func_body_end__ */
  229 
  230 /*****************************************************************/
  237 int example_rx_setup(int unit)
  238 {
  239   /* RX Configuration Structure */
  240   opennsl_rx_cfg_t rx_cfg;
  241   int rv = OPENNSL_E_NONE;
  242   int priority = 10;
  243 
  244   opennsl_rx_cfg_t_init(&rx_cfg);
  245   rx_cfg.pkt_size = 16*1024;
  246   rx_cfg.pkts_per_chain = 15;
  247   rx_cfg.global_pps = 0;
  248   rx_cfg.max_burst = 200;
  249   /* Strip CRC from incoming packets          *
  250    * rx_cfg.flags |= OPENNSL_RX_F_CRC_STRIP;  */
  251   rx_cfg.chan_cfg[1].chains = 4;
  252   /* Packets with all CoS values come through this channel */
  253   rx_cfg.chan_cfg[1].cos_bmp = 0xFFFFFFff;
  254 
  255   /* Register to receive packets punted to CPU */
  256   rv = opennsl_rx_register(unit, "sample application",
  257       example_rx_packet_receive, priority, NULL, OPENNSL_RCO_F_ALL_COS);
  258   if (OPENNSL_FAILURE(rv))
  259   {
  260     printf("Failed to register packet receive callback for unit %d,"
  261         "rv = %d (%s).\r\n", unit, rv, opennsl_errmsg(rv));
  262   }
  263 
  264   if (!opennsl_rx_active(unit))
  265   {
  266     rv = opennsl_rx_start(unit, &rx_cfg);
  267     if (OPENNSL_FAILURE(rv))
  268     {
  269       printf("opennsl_rx_start() failed on unit %d with rv %d (%s).\r\n",
  270           unit, rv, opennsl_errmsg(rv));
  271     }
  272   }
  273   return rv;
  274 }
  275 /* __doxy_func_body_end__ */
  276 
  277 /*****************************************************************/
  284 int main(int argc, char *argv[])
  285 {
  286   opennsl_error_t   rv;
  287   int unit = DEFAULT_UNIT;
  288   int choice;
  289   int index = 0;
  290   opennsl_port_config_t pcfg;
  291 
  292 
  293   if(strcmp(argv[0], "gdb") == 0)
  294   {
  295     index = 1;
  296   }
  297 
  298   if((argc != (index + 1)) || ((argc > (index + 1)) && (strcmp(argv[index + 1], "--help") == 0)))
  299   {
  300     printf("%s\n\r", example_usage);
  301     return OPENNSL_E_PARAM;
  302   }
  303 
  304   /* Initialize the system. */
  305   printf("Initializing the system.\r\n");
  306   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  307 
  308   if(rv != OPENNSL_E_NONE)
  309   {
  310     printf("\r\nFailed to initialize the system. Error %s\r\n",
  311         opennsl_errmsg(rv));
  312     return rv;
  313   }
  314 
  315   rv = example_port_default_config(unit);
  316   if (rv != OPENNSL_E_NONE)
  317   {
  318     printf("\r\nFailed to apply default config on ports, rv = %d (%s).\r\n",
  319            rv, opennsl_errmsg(rv));
  320   }
  321 
  322   /* Add ports to default VLAN. */
  323   printf("Adding ports to default VLAN.\r\n");
  324   rv = example_switch_default_vlan_config(unit);
  325   if(rv != OPENNSL_E_NONE)
  326   {
  327     printf("\r\nFailed to add default ports. rv: %s\r\n", opennsl_errmsg(rv));
  328     return rv;
  329   }
  330 
  331   /* Add CPU port to default VLAN */
  332   rv = opennsl_port_config_get(unit, &pcfg);
  333   if (rv != OPENNSL_E_NONE) {
  334     printf("Failed to get port configuration. Error %s\n", opennsl_errmsg(rv));
  335     return rv;
  336   }
  337 
  338   rv = opennsl_vlan_port_add(unit, DEFAULT_VLAN, pcfg.cpu, pcfg.cpu);
  339   if (rv != OPENNSL_E_NONE) {
  340     printf("Failed to add ports to VLAN. Error %s\n", opennsl_errmsg(rv));
  341     return rv;
  342   }
  343   printf("CPU port is added to default VLAN.\r\n");
  344 
  345   rv = example_rx_setup(unit);
  346   if(rv != OPENNSL_E_NONE)
  347   {
  348     printf("\r\nFailed to setup system for packet reception. rv: %s\r\n",
  349         opennsl_errmsg(rv));
  350     return rv;
  351   }
  352 
  353   while (1)
  354   {
  355     printf("\r\nUser menu: Select one of the following options\r\n");
  356     printf("1. Display OpenNSL version\n");
  357 #ifdef INCLUDE_DIAG_SHELL
  358     printf("9. Launch diagnostic shell\n");
  359 #endif
  360     printf("0. Quit the application\n");
  361 
  362     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  363     {
  364         printf("Invalid option entered. Please re-enter.\n");
  365         continue;
  366     }
  367     switch(choice)
  368     {
  369       case 1:
  370       {
  371         printf("OpenNSL version: %s\n", opennsl_version_get());
  372         break;
  373       }
  374 
  375 #ifdef INCLUDE_DIAG_SHELL
  376       case 9:
  377       {
  378         opennsl_driver_shell();
  379         break;
  380       }
  381 #endif
  382 
  383       case 0:
  384       {
  385         printf("Exiting the application.\n");
  386         rv = opennsl_driver_exit();
  387         return rv;
  388       }
  389 
  390       default:
  391         break;
  392     } /* End of switch */
  393   } /* End of while */
  394 
  395   return OPENNSL_E_NONE;
  396 }
  397 /* __doxy_func_body_end__ */
