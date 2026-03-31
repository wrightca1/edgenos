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
   18  * \file     example_packet_transmit.c
   19  *
   20  * \brief    Example code for packet transmission
   21  *
   22  * \details  This example demonstrates the transmission of a packet
   23  *           through a specified port.
   24  *
   25  ************************************************************************/
   26 
   27 #include <stdio.h>
   28 #include <stdlib.h>
   29 #include <string.h>
   30 #include <sal/driver.h>
   31 #include <sal/version.h>
   32 #include <opennsl/error.h>
   33 #include <opennsl/vlan.h>
   34 #include <opennsl/port.h>
   35 #include <opennsl/switch.h>
   36 #include <opennsl/pkt.h>
   37 #include <opennsl/tx.h>
   38 #include <examples/util.h>
   39 
   40 #define DEFAULT_UNIT  0
   41 #define DEFAULT_VLAN  1
   42 #define MAX_DIGITS_IN_CHOICE 5
   43 
   44 char example_usage[] =
   45 "Syntax: example_packet_transmit                                       \n\r"
   46 "                                                                      \n\r"
   47 "Paramaters: None.                                                     \n\r"
   48 "                                                                      \n\r"
   49 "Example: The following command is used to demonstrate transmit and    \n\r"
   50 "         packet API's.                                                \n\r"
   51 "         example_packet_transmit                                      \n\r"
   52 "                                                                      \n\r"
   53 "Usage Guidelines: None.                                               \n\r";
   54 
   55 /*****************************************************************/
   65 int example_pkt_send(int unit, int port, char *data, int len)
   66 {
   67   opennsl_pkt_t *pkt;
   68   int rv;
   69 
   70   rv = opennsl_pkt_alloc(unit, len, 0, &pkt);
   71 
   72   if (OPENNSL_SUCCESS(rv))
   73   {
   74     rv = opennsl_pkt_memcpy(pkt, 0, data, len);
   75     if (!OPENNSL_SUCCESS(rv))
   76     {
   77       opennsl_pkt_free(unit, pkt);
   78       return rv;
   79     }
   80 
   81     OPENNSL_PBMP_PORT_SET(pkt->tx_pbmp, port);
   82 
   83     rv = opennsl_tx(unit, pkt, NULL);
   84     if (!OPENNSL_SUCCESS(rv))
   85     {
   86       opennsl_pkt_free(unit, pkt);
   87       return rv;
   88     }
   89   }
   90   opennsl_pkt_free(unit, pkt);
   91   return rv;
   92 }
   93 /* __doxy_func_body_end__ */
   94 
   95 
   96 /*****************************************************************/
  105 void example_multi_pkt_send(int unit, int port, int count)
  106 {
  107   int i;
  108   int rv;
  109   char data[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x82, 0x2f,
  110                  0x2e, 0x42, 0x46, 0x74, 0x08, 0x06, 0x00, 0x01,
  111                  0x08, 0x00, 0x06, 0x04, 0x00, 0x01, 0x82, 0x2f,
  112                  0x2e, 0x42, 0x46, 0x74, 0x0a, 0x00, 0x00, 0x01,
  113                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
  114                  0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
  115 
  116   for (i = 0; i < count; i++)
  117   {
  118     rv = example_pkt_send(unit, port, data, (int) sizeof(data));
  119 
  120     if (!OPENNSL_SUCCESS(rv))
  121     {
  122       printf("Error Sending Packet: %d.  Error: %s\n", i + 1, opennsl_errmsg(rv));
  123     }
  124     else
  125     {
  126       printf("Transmitted Packet %d of size %zu\n", i + 1, sizeof(data));
  127     }
  128   }
  129 }
  130 /* __doxy_func_body_end__ */
  131 
  132 /*****************************************************************/
  139 int main(int argc, char *argv[])
  140 {
  141   opennsl_error_t   rv;
  142   int count;
  143   opennsl_port_t port;
  144   int unit = DEFAULT_UNIT;
  145   int choice;
  146   int index = 0;
  147 
  148   if(strcmp(argv[0], "gdb") == 0)
  149   {
  150     index = 1;
  151   }
  152 
  153   if((argc != (index + 1)) || ((argc > (index + 1)) && (strcmp(argv[index + 1], "--help") == 0))) {
  154     printf("%s\n\r", example_usage);
  155     return OPENNSL_E_PARAM;
  156   }
  157 
  158   /* Initialize the system. */
  159   printf("Initializing the system.\r\n");
  160   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  161 
  162   if(rv != OPENNSL_E_NONE) {
  163     printf("\r\nFailed to initialize the system. Error %s\r\n",
  164         opennsl_errmsg(rv));
  165     return rv;
  166   }
  167 
  168   /* cold boot initialization commands */
  169   rv = example_port_default_config(unit);
  170   if (rv != OPENNSL_E_NONE) {
  171     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  172            rv, opennsl_errmsg(rv));
  173   }
  174 
  175   /* Add ports to default vlan. */
  176   printf("Adding ports to default vlan.\r\n");
  177   rv = example_switch_default_vlan_config(unit);
  178   if(rv != OPENNSL_E_NONE) {
  179     printf("\r\nFailed to add default ports. rv: %s\r\n", opennsl_errmsg(rv));
  180     return rv;
  181   }
  182 
  183   while (1) {
  184     printf("\r\nUser menu: Select one of the following options\r\n");
  185     printf("1. Transmit a packet\n");
  186     printf("2. Display OpenNSL version\n");
  187 #ifdef INCLUDE_DIAG_SHELL
  188     printf("9. Launch diagnostic shell\n");
  189 #endif
  190     printf("0. Quit the application\n");
  191 
  192     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  193     {
  194         printf("Invalid option entered. Please re-enter.\n");
  195         continue;
  196     }
  197     switch(choice){
  198 
  199       case 1:
  200       {
  201         printf("\r\nEnter the port on which packet needs to be transmitted.\r\n");
  202         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  203         {
  204             printf("Invalid option entered. Please re-enter.\n");
  205             continue;
  206         }
  207 
  208         printf("\r\nEnter number of packets to be sent.\r\n");
  209         if(example_read_user_choice(&count) != OPENNSL_E_NONE)
  210         {
  211             printf("Invalid option entered. Please re-enter.\n");
  212             continue;
  213         }
  214 
  215         /* Transmit packet(s) on the specified port */
  216         example_multi_pkt_send(unit, port, count);
  217 
  218         break;
  219       } /* End of case 1 */
  220 
  221       case 2:
  222       {
  223         printf("OpenNSL version: %s\n", opennsl_version_get());
  224         break;
  225       }
  226 
  227 #ifdef INCLUDE_DIAG_SHELL
  228       case 9:
  229       {
  230         opennsl_driver_shell();
  231         break;
  232       }
  233 #endif
  234 
  235       case 0:
  236       {
  237         printf("Exiting the application.\n");
  238         rv = opennsl_driver_exit();
  239         return rv;
  240       }
  241 
  242       default:
  243         break;
  244     } /* End of switch */
  245   } /* End of while */
  246 
  247   return OPENNSL_E_NONE;
  248 }
  249 /* __doxy_func_body_end__ */
