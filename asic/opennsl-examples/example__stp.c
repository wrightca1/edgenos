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
   19  * \file         example_stp.c
   20  *
   21  * \brief        OPENNSL example program to demonstrate Spanning Tree
   22  *               application.
   23  *
   24  * \details      This example demonstrates on how to remove a loop by
   25  *               modifying the spanning tree state of a port.
   26  *
   27  *
   28  **********************************************************************/
   29 #include <stdio.h>
   30 #include <stdlib.h>
   31 #include <string.h>
   32 #include <sal/driver.h>
   33 #include <opennsl/types.h>
   34 #include <opennsl/port.h>
   35 #include <opennsl/vlan.h>
   36 #include <opennsl/error.h>
   37 #include <opennsl/stg.h>
   38 #include <examples/util.h>
   39 
   40 char example_usage[] =
   41 "Syntax: example_stp                                                    \n\r"
   42 "                                                                       \n\r"
   43 "Paramaters:                                                            \n\r"
   44 "       port1  - First port number.                                     \n\r"
   45 "       port2  - Second port number.                                    \n\r"
   46 "                                                                       \n\r"
   47 "Example: The following command is used to demonstrate on how to remove \n\r"
   48 "         loop by modifying the spanning tree state of a port.          \n\r"
   49 "         example_stp 5 6                                               \n\r"
   50 "                                                                       \n\r"
   51 "Usage Guidelines: This program takes two ports numbers as input and    \n\r"
   52 "       place them in VLAN 100 and by default put them in forwarding    \n\r"
   53 "       state. This results in a loop if both the ports are connected   \n\r"
   54 "       to a partner switch. Here the program provides the menu with    \n\r"
   55 "       the following options.                                          \n\r"
   56 "                                                                       \n\r"
   57 "       1) Configure the STP state of a port.                           \n\r"
   58 "       2) Display the STP state of ports.                              \n\r"
   59 "       3) Quit the application.                                        \n\r"
   60 "                                                                       \n\r";
   61 
   62 
   63 #define DEFAULT_UNIT         0
   64 #define VLANID               100
   65 #define MAX_DIGITS_IN_CHOICE 5
   66 
   67 typedef struct {
   68   int vlan_1;
   69   int port_1;
   70   int port_2;
   71   int stp_state_1;
   72   int stp_state_2;
   73   opennsl_stg_t stg;
   74 } stg_info_s;
   75 /* __doxy_func_body_end__ */
   76 
   77 char *stp_state_str[] =
   78 {
   79   "Disable", /* OPENNSL_STG_STP_DISABLE */
   80   "Block",   /* OPENNSL_STG_STP_BLOCK   */
   81   "Listen",  /* OPENNSL_STG_STP_LISTEN  */
   82   "Learn",   /* OPENNSL_STG_STP_LEARN   */
   83   "Forward"  /* OPENNSL_STG_STP_FORWARD */
   84 };
   85 /* __doxy_func_body_end__ */
   86 
   87 /* debug prints */
   88 int verbose = 3;
   89 
   90 #define CALL_IF_ERROR_RETURN(op)                            \
   91   do {                                                      \
   92     int __rv__;                                             \
   93     if ((__rv__ = (op)) < 0) {                              \
   94       printf("%s:%s: line %d rv: %d failed: %s\n",          \
   95           __FILE__, __FUNCTION__, __LINE__, __rv__,         \
   96           opennsl_errmsg(__rv__));                          \
   97     }                                                       \
   98   } while(0)
   99 /* __doxy_func_body_end__ */
  100 
  101 
  102 /* Declarations below */
  103 int example_stg_init(int unit, stg_info_s *info, int port1, int port2);
  104 int example_stg_create(int unit, stg_info_s *info);
  105 
  106 /**********************************************************************/
  113 int main(int argc, char *argv[])
  114 {
  115   int rv = OPENNSL_E_NONE;
  116   int unit = DEFAULT_UNIT;
  117   int port1;
  118   int port2;
  119   int port;
  120   int stp_mode;
  121   int stp_state;
  122   int choice;
  123   stg_info_s info;
  124 
  125 
  126   if((argc != 3) || ((argc > 1) && (strcmp(argv[1], "--help") == 0))) {
  127     printf("%s\n\r", example_usage);
  128     return OPENNSL_E_PARAM;
  129   }
  130 
  131   /* Initialize the system */
  132   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  133 
  134   if(rv != OPENNSL_E_NONE) {
  135     printf("\r\nFailed to initialize the system.\r\n");
  136     return rv;
  137   }
  138 
  139   /* cold boot initialization commands */
  140   rv = example_port_default_config(unit);
  141   if (rv != OPENNSL_E_NONE) {
  142     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  143            rv, opennsl_errmsg(rv));
  144   }
  145 
  146   /* Extract inputs parameters */
  147   port1 = atoi(argv[1]);
  148   port2 = atoi(argv[2]);
  149 
  150   /* Call the STP action routines */
  151   CALL_IF_ERROR_RETURN(example_stg_init(unit, &info, port1, port2));
  152   CALL_IF_ERROR_RETURN(example_stg_create(unit, &info));
  153 
  154   /* Display the interactive user menu */
  155   while(1)
  156   {
  157     printf("Interactive User Menu:\n");
  158     printf("1. Configure STP state of a port\n");
  159     printf("2. Display the STP state of ports\n");
  160 #ifdef INCLUDE_DIAG_SHELL
  161     printf("9. Launch diagnostic shell\n");
  162 #endif
  163     printf("0. Quit the application\n");
  164 
  165    if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  166    {
  167        printf("Invalid option entered. Please re-enter.\n");
  168        continue;
  169     }
  170 
  171     switch(choice)
  172     {
  173       case 1:
  174       {
  175         printf("1. Enter port number\n");
  176         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  177         {
  178             printf("Invalid option entered. Please re-enter.\n");
  179             continue;
  180         }
  181         printf("1. Enter STP state of the port\n");
  182         printf("   STP state: 1 - Forward, 2 - Block\n");
  183         if(example_read_user_choice(&stp_mode) != OPENNSL_E_NONE)
  184         {
  185             printf("Invalid option %d entered. Please re-enter.\n", stp_mode);
  186             continue;
  187         }
  188 
  189         if(stp_mode == 1) {
  190           stp_state = OPENNSL_STG_STP_FORWARD;
  191         } else if(stp_mode == 2) {
  192           stp_state = OPENNSL_STG_STP_BLOCK;
  193         }
  194         else {
  195           printf("Invalid option is entered. Please re-enter.\n");
  196           continue;
  197         }
  198 
  199         rv = opennsl_stg_stp_set(unit, info.stg, port, stp_state);
  200         if (rv != OPENNSL_E_NONE) {
  201           printf("Failed to set STP state of port %d to "
  202               "\"%s\" in STG %d, \n",
  203               port, stp_state_str[stp_state], info.stg);
  204           return rv;
  205         }
  206         if(verbose >= 2) {
  207           printf("STP state of port %d is set to \"%s\".\n",
  208               port, stp_state_str[stp_state]);
  209         }
  210         break;
  211       }
  212       case 2:
  213       {
  214         rv = opennsl_stg_stp_get(unit, info.stg, info.port_1, &stp_state);
  215         if (rv != OPENNSL_E_NONE) {
  216           printf("Failed to get STP state of port %d in STG %d, \n",
  217               port, info.stg);
  218           return rv;
  219         }
  220         printf("STP state of port %d is \"%s\" in STG %d.\n",
  221             info.port_1, stp_state_str[stp_state], info.stg);
  222 
  223         rv = opennsl_stg_stp_get(unit, info.stg, info.port_2, &stp_state);
  224         if (rv != OPENNSL_E_NONE) {
  225           printf("Failed to get STP state of port %d in STG %d, \n",
  226               info.port_2, info.stg);
  227           return rv;
  228         }
  229         printf("STP state of port %d is \"%s\" in STG %d.\n",
  230             info.port_2, stp_state_str[stp_state], info.stg);
  231         break;
  232       }
  233 
  234 #ifdef INCLUDE_DIAG_SHELL
  235       case 9:
  236       {
  237         opennsl_driver_shell();
  238         break;
  239       }
  240 #endif
  241 
  242       case 0:
  243       {
  244         printf("Exiting the application.\n");
  245         rv = opennsl_driver_exit();
  246         return rv;
  247       }
  248       default:
  249         printf("Invalid option is entered. Please re-enter.\n");
  250 
  251     } /* End of switch */
  252 
  253   } /* End of while */
  254 
  255 
  256   return rv;
  257 }
  258 /* __doxy_func_body_end__ */
  259 
  260 /**************************************************************************/
  273 int example_stg_init(int unit, stg_info_s *info, int port1, int port2) {
  274   int rv;
  275 
  276   info->vlan_1 = VLANID;
  277   info->port_1 = port1;
  278   info->port_2 = port2;
  279   info->stp_state_1 = OPENNSL_STG_STP_FORWARD;
  280   info->stp_state_2 = OPENNSL_STG_STP_FORWARD;
  281   rv = opennsl_stg_create(unit,&(info->stg));
  282   return rv;
  283 }
  284 /* __doxy_func_body_end__ */
  285 
  286 /**************************************************************************/
  295 int example_stg_create(int unit, stg_info_s *info) {
  296   int rv;
  297   opennsl_pbmp_t pbmp;
  298 
  299   /* create VLAN */
  300   rv = opennsl_vlan_create(DEFAULT_UNIT, info->vlan_1);
  301   if (rv != OPENNSL_E_NONE) {
  302     printf("Error, in opennsl_vlan_create, vid=%d, \n", info->vlan_1);
  303     return rv;
  304   }
  305 
  306   if(verbose >= 2) {
  307     printf("VLAN %d is created.\n", info->vlan_1);
  308   }
  309 
  310   OPENNSL_PBMP_CLEAR(pbmp);
  311   OPENNSL_PBMP_PORT_ADD(pbmp, info->port_1);
  312   OPENNSL_PBMP_PORT_ADD(pbmp, info->port_2);
  313 
  314   rv = opennsl_vlan_port_add(unit, info->vlan_1, pbmp, pbmp);
  315   if (rv != OPENNSL_E_NONE) {
  316     printf("OPENNSL FAIL %d: %s\n", rv, opennsl_errmsg(rv));
  317     return rv;
  318   }
  319 
  320   if(verbose >= 2) {
  321     printf("Added ports %d, %d to VLAN %d.\n",
  322            info->port_1, info->port_2, info->vlan_1);
  323   }
  324 
  325   /* add the vlans to the stg */
  326   rv = opennsl_stg_vlan_add(unit, info->stg, info->vlan_1);
  327   if (rv != OPENNSL_E_NONE) {
  328     printf("Error, in opennsl_stg_vlan_add, vid=%d, \n", info->vlan_1);
  329     return rv;
  330   }
  331 
  332   if(verbose >= 2) {
  333     printf("Attached VLAN %d to Spanning Tree Group %d.\n",
  334            info->vlan_1, info->stg);
  335   }
  336 
  337   /* add two ports to the stg and attach stp state for each of them */
  338   rv = opennsl_stg_stp_set(unit, info->stg, info->port_1, info->stp_state_1);
  339   if (rv != OPENNSL_E_NONE) {
  340     printf("Error, in opennsl_stg_stp_set, stg=%d, \n", info->stg);
  341     return rv;
  342   }
  343 
  344   if(verbose >= 2) {
  345     printf("STP state of port %d is set to \"%s\".\n", 
  346            info->port_1, stp_state_str[info->stp_state_1]);
  347   }
  348 
  349   rv = opennsl_stg_stp_set(unit, info->stg, info->port_2, info->stp_state_2);
  350   if (rv != OPENNSL_E_NONE) {
  351     printf("Error, in opennsl_stg_stp_set, stg=%d, \n", info->stg);
  352     return rv;
  353   }
  354   if(verbose >= 2) {
  355     printf("STP state of port %d is set to \"%s\".\n",
  356            info->port_2, stp_state_str[info->stp_state_2]);
  357   }
  358 
  359   return OPENNSL_E_NONE;
  360 }
  361 /* __doxy_func_body_end__ */
  362 
  363 /**************************************************************************/
  372 int revert_stg(int unit, stg_info_s *info) {
  373   int rv = OPENNSL_E_NONE;
  374 
  375   rv = opennsl_stg_destroy(unit,info->stg);
  376   if (rv != OPENNSL_E_NONE) {
  377     printf("Error, in opennsl_stg_destroy\n");
  378     return rv;
  379   }
  380 
  381   rv = opennsl_vlan_destroy(unit,info->vlan_1);
  382   if (rv != OPENNSL_E_NONE) {
  383     printf("Error, in opennsl_vlan_destroy, vid=%d, \n", info->vlan_1);
  384     return rv;
  385   }
  386 
  387   return rv;
  388 }
  389 /* __doxy_func_body_end__ */
