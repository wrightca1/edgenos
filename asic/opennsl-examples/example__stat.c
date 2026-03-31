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
   18  *
   19  * \file         example_stat.c
   20  *
   21  * \brief        OpenNSL example program for showing statistics
   22  *
   23  * \details      OpenNSL example program for displaying and clearing statistics
   24  *               of a given interface.
   25  *               interface.
   26  *
   27  **********************************************************************/
   28 #include <stdio.h>
   29 #include <stdlib.h>
   30 #include <string.h>
   31 #include <sal/driver.h>
   32 #include <opennsl/error.h>
   33 #include <opennsl/stat.h>
   34 #include <opennsl/vlan.h>
   35 #include <opennsl/stat.h>
   36 #include <examples/util.h>
   37 
   38 #define DEFAULT_UNIT 0
   39 #define DEFAULT_VLAN 1
   40 #define MAX_COUNTERS 30
   41 #define MAX_DIGITS_IN_CHOICE 5
   42 
   43 char example_usage[] =
   44 "Syntax: example_stat                                                  \n\r"
   45 "                                                                      \n\r"
   46 "Paramaters: None                                                      \n\r"
   47 "                                                                      \n\r"
   48 "Example: The following command is used to see the statistics of a port\n\r"
   49 "         example_stat                                                 \n\r"
   50 "                                                                      \n\r"
   51 "Usage Guidelines: This program request the user to enter the port     \n\r"
   52 "                  number interactively                                \n\r";
   53 
   54 /*****************************************************************/
   61 /* Main function for Statistics application */
   62 int main(int argc, char *argv[])
   63 {
   64   opennsl_error_t   rc;
   65   int choice;
   66   int unit = DEFAULT_UNIT;
   67   int i;
   68   int nstat;
   69   opennsl_port_t port;
   70   opennsl_stat_val_t stat_arr[] = {
   71     opennsl_spl_snmpIfInUcastPkts,
   72     opennsl_spl_snmpIfInNUcastPkts,
   73     opennsl_spl_snmpIfInDiscards,
   74     opennsl_spl_snmpIfInErrors,
   75     opennsl_spl_snmpIfInMulticastPkts,
   76     opennsl_spl_snmpIfInBroadcastPkts,
   77     snmpOpenNSLReceivedPkts64Octets,
   78     snmpOpenNSLReceivedPkts65to127Octets,
   79     snmpOpenNSLReceivedPkts128to255Octets,
   80     snmpOpenNSLReceivedPkts256to511Octets,
   81     snmpOpenNSLReceivedPkts512to1023Octets,
   82     snmpOpenNSLReceivedPkts1024to1518Octets,
   83     snmpOpenNSLReceivedPkts1519to2047Octets,
   84     snmpOpenNSLReceivedPkts2048to4095Octets,
   85     snmpOpenNSLReceivedPkts4095to9216Octets,
   86     opennsl_spl_snmpIfOutUcastPkts,
   87     opennsl_spl_snmpIfOutNUcastPkts,
   88     opennsl_spl_snmpIfOutDiscards,
   89     opennsl_spl_snmpIfOutErrors,
   90     opennsl_spl_snmpIfOutMulticastPkts,
   91     opennsl_spl_snmpIfOutBroadcastPkts,
   92     snmpOpenNSLTransmittedPkts64Octets,
   93     snmpOpenNSLTransmittedPkts65to127Octets,
   94     snmpOpenNSLTransmittedPkts128to255Octets,
   95     snmpOpenNSLTransmittedPkts256to511Octets,
   96     snmpOpenNSLTransmittedPkts512to1023Octets,
   97     snmpOpenNSLTransmittedPkts1024to1518Octets,
   98     snmpOpenNSLTransmittedPkts1519to2047Octets,
   99     snmpOpenNSLTransmittedPkts2048to4095Octets,
  100     snmpOpenNSLTransmittedPkts4095to9216Octets,};
  101   uint64 val[MAX_COUNTERS] = {0};
  102 
  103   nstat = (sizeof(stat_arr) / sizeof(opennsl_stat_val_t));
  104 
  105   if((argc != 1) || ((argc > 1) && (strcmp(argv[1], "--help") == 0))) {
  106     printf("%s\n\r", example_usage);
  107     return OPENNSL_E_PARAM;
  108   }
  109 
  110   /* Initialize the system. */
  111   printf("Initializing the system.\r\n");
  112   rc = opennsl_driver_init((opennsl_init_t *) NULL);
  113 
  114   if(rc != OPENNSL_E_NONE) {
  115     printf("\r\nFailed to initialize the system, rc = %d (%s).\r\n",
  116            rc, opennsl_errmsg(rc));
  117     return 0;
  118   }
  119 
  120   /* cold boot initialization commands */
  121   rc = example_port_default_config(unit);
  122   if (rc != OPENNSL_E_NONE) {
  123     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  124            rc, opennsl_errmsg(rc));
  125   }
  126 
  127   /* Add ports to default vlan. */
  128   printf("Adding ports to default vlan.\r\n");
  129   rc = example_switch_default_vlan_config(unit);
  130   if(rc != OPENNSL_E_NONE) {
  131     printf("\r\nFailed to add default ports, rc = %d (%s).\r\n",
  132            rc, opennsl_errmsg(rc));
  133   }
  134 
  135   /* Initialize stat module. */
  136   printf("Initializing the stat module.\r\n");
  137   rc = opennsl_stat_init(unit);
  138 
  139   if(rc != OPENNSL_E_NONE) {
  140     printf("\r\nFailed to initialise stat module, rc = %d (%s).\r\n",
  141            rc, opennsl_errmsg(rc));
  142     return 0;
  143   }
  144 
  145 
  146   while (1) {
  147     printf("\nUser Menu: Select one of the following options\n");
  148     printf("1. Display statistics of a port.\n");
  149     printf("2. Clear statistics of a port.\n");
  150 #ifdef INCLUDE_DIAG_SHELL
  151     printf("9. Launch diagnostic shell\n");
  152 #endif
  153     printf("0. Quit the application.\n");
  154 
  155     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  156     {
  157         printf("Invalid option entered. Please re-enter.\n");
  158         continue;
  159     }
  160     switch(choice) {
  161 
  162       case 1:
  163       {
  164         printf("\r\nEnter the port number.\r\n");
  165         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  166         {
  167             printf("Invalid option entered. Please re-enter.\n");
  168             continue;
  169         }
  170         rc = (opennsl_stat_sync(unit));
  171         if(rc != OPENNSL_E_NONE) {
  172           printf("\r\nFailed to sync the state of port, rc = %d (%s).\r\n",
  173                  rc, opennsl_errmsg(rc));
  174           break;
  175         }
  176 
  177         rc = opennsl_stat_multi_get(unit, port, nstat, stat_arr, val);
  178  
  179         if (rc == OPENNSL_E_UNAVAIL) {
  180            for (i=0; i < nstat; i++) {
  181               rc = opennsl_stat_get(unit, port, stat_arr[i], &val[i]);
  182            }          
  183         }
  184 
  185         if(rc != OPENNSL_E_NONE) {
  186           printf("\r\nFailed to get the port stats, rc = %d (%s).\r\n",
  187                  rc, opennsl_errmsg(rc));
  188           break;
  189         }
  190 
  191         printf("Count of Ingress Unicast packets    .......   %llu\n", val[0]);
  192         printf("Count of Ingress Non-unicast packets.......   %llu\n", val[1]);
  193         printf("Count of Ingress Discard packets    .......   %llu\n", val[2]);
  194         printf("Count of Ingress Error packets      .......   %llu\n", val[3]);
  195         printf("Count of Ingress Multicast packets  .......   %llu\n", val[4]);
  196         printf("Count of Ingress Broadcast packets  .......   %llu\n", val[5]);
  197         printf("Count of Received 64 octets         .......   %llu\n", val[6]);
  198         printf("Count of Received 65-127 octets     .......   %llu\n", val[7]);
  199         printf("Count of Received 128-255 octets    .......   %llu\n", val[8]);
  200         printf("Count of Received 256-511 octets    .......   %llu\n", val[9]);
  201         printf("Count of Received 512-1023 octets   .......   %llu\n", val[10]);
  202         printf("Count of Received 1024-1518 octets  .......   %llu\n", val[11]);
  203         printf("Count of Received 1519-2047 octets  .......   %llu\n", val[12]);
  204         printf("Count of Received 2048-4095 octets  .......   %llu\n", val[13]);
  205         printf("Count of Received 4096-9216 octets  .......   %llu\n", val[14]);
  206         printf("Count of Egress Unicast packets     .......   %llu\n", val[15]);
  207         printf("Count of Egress Non-unicast packets .......   %llu\n", val[16]);
  208         printf("Count of Egress Discards packets    .......   %llu\n", val[17]);
  209         printf("Count of Egress Error packets       .......   %llu\n", val[18]);
  210         printf("Count of Egress Multicast packets   .......   %llu\n", val[19]);
  211         printf("Count of Egress Broadcast packets   .......   %llu\n", val[20]);
  212         printf("Count of Transmitted 64 octets      .......   %llu\n", val[21]);
  213         printf("Count of Transmitted 65-127 octets  .......   %llu\n", val[22]);
  214         printf("Count of Transmitted 128-255 octets .......   %llu\n", val[23]);
  215         printf("Count of Transmitted 256-511 octets .......   %llu\n", val[24]);
  216         printf("Count of Transmitted 512-1023 octet .......   %llu\n", val[25]);
  217         printf("Count of Transmitted 1024-1518 octets .....   %llu\n", val[26]);
  218         printf("Count of Transmitted 1519-2047 octets .....   %llu\n", val[27]);
  219         printf("Count of Transmitted 2048-4095 octets .....   %llu\n", val[28]);
  220         break;
  221       } /* End of case 1 */
  222 
  223       case 2:
  224       {
  225         printf("\r\nEnter the port number.\r\n");
  226         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  227         {
  228             printf("Invalid option entered. Please re-enter.\n");
  229             continue;
  230         }
  231         rc = opennsl_stat_clear(unit, port);
  232         if(rc != OPENNSL_E_NONE) {
  233           printf("\r\nFailed to clear the port stats, rc = %d (%s).\r\n",
  234                  rc, opennsl_errmsg(rc));
  235           break;
  236         }
  237 
  238         printf("\r\nPort %d stats cleared\r\n", port);
  239         break;
  240       } /* End of case 2 */
  241 
  242 #ifdef INCLUDE_DIAG_SHELL
  243       case 9:
  244       {
  245         opennsl_driver_shell();
  246         break;
  247       }
  248 #endif
  249 
  250       case 0:
  251       {
  252         printf("Exiting the application.\n");
  253         rc = opennsl_driver_exit();
  254         return rc;
  255       }
  256 
  257       default :
  258         break;
  259     } /* End of switch */
  260   } /* End of while */
  261 
  262   return 0;
  263 }
  264 /* __doxy_func_body_end__ */
  265 
