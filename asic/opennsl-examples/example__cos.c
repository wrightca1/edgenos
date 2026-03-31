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
   19  * \file         example_cos.c
   20  *
   21  * \brief        OpenNSL example application for Class Of Service module
   22  *
   23  * \details       This example includes:
   24  *  o     Port shaper 
   25  *  o     Queue's weight (WFQ) 
   26  *  o     Strict priority 
   27  *  o     Incoming TC/DP mapping 
   28  *
   29  * It is assumed diag_init is executed.
   30  * 
   31  * Settings include:
   32  * Set the OFP Bandwidth to 5G using the port shaper.
   33  *  1.  Set the OFP Bandwidth to 5G using the port shaper. 
   34  *    2.    Set queues priorities WFQ/SP as follows:
   35  *         -    For the high-priority queues, WFQ is set: UC will get 2/3 of the BW and MC will get 1/3 of the BW.
   36  *         -    For the low-priority queues, SP UC is set over MC.
   37  *    3.    Configure TC/DP egress mapping. Map low-priority traffic (0 - 3 for UC, 0  - 5 for MC)
   38  *     to low-priority queues, and high-priority traffic (4  - 7 for UC, 6  - 7 for MC) to high-priority queues.
   39  * 
   40  *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   41  *  |                                                                        |
   42  *  |                                   |\                                   |
   43  *  |opennslCosqGportTypeUnicastEgress  | \                                  |
   44  *  |             +-----------+         |  \                                 |
   45  *  |   TC 0-3 -->| HP UC  || |---------|2/3\                                |
   46  *  |             +-----------+         |    |                               |
   47  *  |opennslCosqGportTypeMulticastEgress|WFQ |-----+                         |
   48  *  |             +-----------+         |    |     |     |\                  |
   49  *  |   TC 0-5 -->| HP MC  || |---------|1/3/      |     | \                 |
   50  *  |             +-----------+         |  /       +---->|H \                |
   51  *  |                                   | /              |   \               |
   52  *  |                                   |/               |    |    5G        |
   53  *  |                                                    | SP |----SPR---->  |
   54  *  |opennslCosqGportTypeUnicastEgress  |\               |    |              |
   55  *  |             +-----------+         | \              |   /               |
   56  *  |   TC 4-7 -->| LP UC  || |---------|H \       +---->|L /                |
   57  *  |             +-----------+         |   |      |     | /                 |
   58  *  |opennslCosqGportTypeMulticastEgress| SP|------+     |/                  |
   59  *  |             +-----------+         |   |                                |
   60  *  |   TC 6-7 -->| LP MC  || |---------|L /          +----------------+     |
   61  *  |             +-----------+         | /           |      KEY       |     |
   62  *  |                                   |/            +----------------+     |
   63  *  |                                                 |SPR- Rate Shaper|     |
   64  *  |                                                 |                |     |
   65  *  |                                                 +----------------+     |
   66  *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   67  * 
   68  */
   69 
   70 #include <stdio.h>
   71 #include <stdlib.h>
   72 #include <string.h>
   73 #include <opennsl/error.h>
   74 #include <opennsl/init.h>
   75 #include <opennsl/stack.h>
   76 #include <opennsl/port.h>
   77 #include <opennsl/l2.h>
   78 #include <opennsl/vlan.h>
   79 #include <opennsl/cosq.h>
   80 #include <opennsl/multicast.h>
   81 #include <examples/util.h>
   82 
   83 
   84 char example_usage[] =
   85 "Syntax: example_cos                                                    \n\r"
   86 "                                                                       \n\r"
   87 "Paramaters: None.                                                      \n\r"
   88 "                                                                       \n\r"
   89 "Example: The following command is used to test the CoS functionality   \n\r"
   90 "                                                                       \n\r"
   91 "         example_cos                                                   \n\r"
   92 "                                                                       \n\r"
   93 "Usage Guidelines: None.                                                \n\r";
   94 
   95 
   96 #define DEFAULT_UNIT 0
   97 #define DEFAULT_VLAN 1
   98 
   99 
  100 int egress_mc = 0;
  101 
  102 int multicast__open_mc_group(int unit, int *mc_group_id, int extra_flags) 
  103 {
  104   int rv = OPENNSL_E_NONE;
  105   int flags;
  106 
  107   /* destroy before open, to ensure it not exist */
  108   rv = opennsl_multicast_destroy(unit, *mc_group_id);
  109 
  110   printf("egress_mc: %d \n", egress_mc);
  111 
  112   flags = OPENNSL_MULTICAST_WITH_ID | extra_flags;
  113   /* create ingress/egress MC */
  114   if (egress_mc) 
  115   {
  116     flags |= OPENNSL_MULTICAST_EGRESS_GROUP;
  117   } 
  118   else 
  119   {
  120     flags |= OPENNSL_MULTICAST_INGRESS_GROUP;
  121   }
  122 
  123   rv = opennsl_multicast_create(unit, flags, mc_group_id);
  124   if (rv != OPENNSL_E_NONE) 
  125   {
  126     printf("Error, opennsl_multicast_create, flags $flags mc_group_id $mc_group_id \n");
  127     return rv;
  128   }
  129 
  130   printf("Created mc_group %d \n", *mc_group_id);
  131 
  132   return rv;
  133 }
  134 /* __doxy_func_body_end__ */
  135 
  136 /* Adding entries to MC group
  137  * ipmc_index:  mc group
  138  * ports: array of ports to add to the mc group
  139  * cud 
  140  * nof_mc_entries: number of entries to add to the mc group
  141  * is_ingress:  if true, add ingress mc entry, otherwise, add egress mc 
  142  * see add_ingress_multicast_forwarding from cint_ipmc_flows.c
  143  * 
  144    */
  145 int multicast__add_multicast_entry(int unit, int ipmc_index, int *ports, int *cud, int nof_mc_entries, int is_egress) 
  146 {
  147   int rv = OPENNSL_E_NONE;
  148   int i;
  149 
  150   for (i=0;i<nof_mc_entries;i++) 
  151   {
  152     /* egress MC */
  153     if (is_egress) 
  154     {
  155       rv = opennsl_multicast_egress_add(unit,ipmc_index,ports[i],cud[i]);
  156       if (rv != OPENNSL_E_NONE) 
  157       {
  158         printf("Error, opennsl_multicast_egress_add: port %d encap_id: %d \n", ports[i], cud[i]);
  159         return rv;
  160       }
  161     } 
  162     /* ingress MC */
  163     else 
  164     {
  165       rv = opennsl_multicast_ingress_add(unit,ipmc_index,ports[i],cud[i]);
  166       if (rv != OPENNSL_E_NONE) 
  167       {
  168         printf("Error, opennsl_multicast_ingress_add: port %d encap_id: %d \n", ports[i], cud[i]);
  169         return rv;
  170       }
  171     }
  172   }
  173 
  174   return rv;
  175 }
  176 /* __doxy_func_body_end__ */
  177 
  178 int multicast__open_egress_mc_group_with_local_ports(int unit, int mc_group_id, int *dest_local_port_id, int *cud, int num_of_ports, int extra_flags) 
  179 {
  180   int i;
  181   opennsl_error_t rv = OPENNSL_E_NONE;
  182   opennsl_cosq_gport_info_t gport_info;
  183   opennsl_cosq_gport_type_t gport_type = opennslCosqGportTypeLocalPort;
  184 
  185   egress_mc = 1;
  186 
  187   rv = multicast__open_mc_group(unit, &mc_group_id, extra_flags);
  188   if (rv != OPENNSL_E_NONE) 
  189   {
  190     printf("Error, multicast__open_mc_group, extra_flags $extra_flags mc_group $mc_group_id \n");
  191     return rv;
  192   }
  193 
  194   for(i=0;i<num_of_ports;i++) 
  195   {
  196     OPENNSL_GPORT_LOCAL_SET(gport_info.in_gport,dest_local_port_id[i]); 
  197     rv = opennsl_cosq_gport_handle_get(unit,gport_type,&gport_info);
  198     if (rv != OPENNSL_E_NONE) 
  199     {
  200       printf("Error, opennsl_cosq_gport_handle_get, gport_type $gport_type \n");
  201       return rv;
  202     }
  203 
  204     rv = multicast__add_multicast_entry(unit, mc_group_id, &gport_info.out_gport, &cud[i], 1, egress_mc);
  205     if (rv != OPENNSL_E_NONE) 
  206     {
  207       printf("Error, multicast__add_multicast_entry, mc_group_id $mc_group_id dest_gport $gport_info.out_gport \n");
  208       return rv;
  209     }
  210   }
  211 
  212   return rv;
  213 }
  214 /* __doxy_func_body_end__ */
  215 
  216 /* Set OFP Bandwidth (max KB per sec)
  217  * Configure the Port Shaper's max rate
  218  */
  219 int example_set_ofp_bandwidth(int unit, int local_port_id, int max_kbits_sec)
  220 {
  221   opennsl_error_t rv = OPENNSL_E_NONE;
  222   opennsl_cosq_gport_type_t gport_type;
  223   opennsl_cosq_gport_info_t gport_info;
  224   opennsl_gport_t out_gport;
  225 
  226   int min_kbits_rate = 0;
  227   int flags = 0;
  228 
  229   /* Set GPORT according to the given local_port_id */
  230   OPENNSL_GPORT_LOCAL_SET(gport_info.in_gport, local_port_id); 
  231 
  232   gport_type = opennslCosqGportTypeLocalPort; 
  233 
  234   rv = opennsl_cosq_gport_handle_get(unit, gport_type, &gport_info);
  235   if (rv != OPENNSL_E_NONE) 
  236   {
  237     printf("Error, in handle get, gport_type $gport_type \n");
  238     return rv;
  239   }
  240 
  241   out_gport = gport_info.out_gport;
  242   rv = opennsl_cosq_gport_bandwidth_set(unit, out_gport, 0, min_kbits_rate, max_kbits_sec, flags);
  243   if (rv != OPENNSL_E_NONE) 
  244   {
  245     printf("Error, in bandwidth set, out_gport $out_gport max_kbits_sec $max_kbits_sec \n");
  246     return rv;
  247   }
  248 
  249   return rv;
  250 }
  251 /* __doxy_func_body_end__ */
  252 
  253 /* Set Strict Priority Configuration 
  254  * queue = OPENNSL_COSQ_LOW_PRIORITY
  255  *         OPENNSL_COSQ_HIGH_PRIORITY
  256  * SP_Type = 0 for UC over MC
  257  *           1 for MC over UC
  258  */
  259 int example_set_sp(int unit, int local_port_id, int queue, int sp_type)
  260 {
  261   opennsl_error_t rv = OPENNSL_E_NONE;
  262   opennsl_cosq_gport_type_t gport_type;
  263   opennsl_cosq_gport_info_t gport_info;
  264   opennsl_gport_t out_gport;
  265 
  266   /* Set GPORT according to the given local_port_id */
  267   OPENNSL_GPORT_LOCAL_SET(gport_info.in_gport,local_port_id); 
  268 
  269   /* We will define MC as high or low priority */
  270   gport_type = opennslCosqGportTypeMulticastEgress; 
  271 
  272   rv = opennsl_cosq_gport_handle_get(unit,gport_type,&gport_info);
  273   if (rv != OPENNSL_E_NONE) 
  274   {
  275     printf("Error, in handle get, gport_type $gport_type \n");
  276     return rv;
  277   }
  278 
  279   out_gport = gport_info.out_gport;
  280   if(sp_type == 0) 
  281   {
  282     /* Setting with OPENNSL_COSQ_SP1 will set the MC to be LOWER priority --> UC over MC */
  283     rv = opennsl_cosq_gport_sched_set(unit, out_gport, queue, OPENNSL_COSQ_SP1, -1);
  284   } 
  285   else 
  286   {
  287     /* Setting with OPENNSL_COSQ_SP0 will set the MC to be HIGHER priority --> MC over UC */
  288     rv = opennsl_cosq_gport_sched_set(unit, out_gport, queue, OPENNSL_COSQ_SP0, -1);
  289   }
  290 
  291   if (rv != OPENNSL_E_NONE) 
  292   {
  293     printf("Error, in SP set, out_gport $out_gport queue $queue sp_type  $sp_type \n");
  294   }
  295 
  296   return rv;
  297 }
  298 /* __doxy_func_body_end__ */
  299 
  300 /* Generic set weight function 
  301  * queue = OPENNSL_COSQ_LOW_PRIORITY
  302  *         OPENNSL_COSQ_HIGH_PRIORITY
  303  * uc_mc = 0 for UC
  304  *         1 for MC 
  305  */
  306 int example_set_weight(int unit, int local_port_id, int queue, int uc_mc, int weight)
  307 {
  308   opennsl_error_t rv = OPENNSL_E_NONE;
  309   opennsl_cosq_gport_type_t gport_type;
  310   opennsl_cosq_gport_info_t gport_info;
  311   opennsl_gport_t out_gport;
  312 
  313   /* Set GPORT according to the given local_port_id */
  314   OPENNSL_GPORT_LOCAL_SET(gport_info.in_gport,local_port_id); 
  315 
  316   if(uc_mc == 0) 
  317   {
  318     gport_type = opennslCosqGportTypeUnicastEgress; 
  319   } 
  320   else 
  321   {
  322     gport_type = opennslCosqGportTypeMulticastEgress; 
  323   }
  324 
  325   rv = opennsl_cosq_gport_handle_get(unit,gport_type,&gport_info);
  326   if (rv != OPENNSL_E_NONE) 
  327   {
  328     printf("Error, in handle get, gport_type $gport_type \n");
  329     return rv;
  330   }
  331 
  332   out_gport = gport_info.out_gport;
  333 
  334   /* Set the weight of the designated queue
  335    * mode = -1 becuase we are setting weight and not strict priority
  336    */
  337   rv = opennsl_cosq_gport_sched_set(unit, out_gport, queue, -1, weight);
  338   if (rv != OPENNSL_E_NONE) 
  339   {
  340     printf("Error, in weight set, out_gport $out_gport queue $queue weight $weight \n");
  341   }
  342 
  343   return rv;
  344 }
  345 /* __doxy_func_body_end__ */
  346 
  347 int
  348 _opennsl_petra_egress_queue_from_cosq(int unit,
  349                                       int *queue_id,
  350                                       int cosq)
  351 {
  352   opennsl_error_t rv = OPENNSL_E_NONE;
  353 
  354   switch (cosq) 
  355   {
  356     case OPENNSL_COSQ_HIGH_PRIORITY:
  357     case 0:
  358         *queue_id = 0;
  359         break;
  360     case OPENNSL_COSQ_LOW_PRIORITY:
  361     case 1:
  362         *queue_id = 1;
  363         break;
  364     case 2:
  365     case 3:
  366     case 4:
  367     case 5:
  368     case 6:
  369     case 7:
  370         *queue_id = cosq;
  371         break;
  372   }
  373 
  374   return rv;
  375 }
  376 /* __doxy_func_body_end__ */
  377 
  378 /* Map incoming UC TC/DP to L/H queue
  379  *  incoming_tc = 0-7
  380  *  incoming_dp = 0-3
  381  *  queue = OPENNSL_COSQ_LOW_PRIORITY
  382  *          OPENNSL_COSQ_HIGH_PRIORITY
  383  */
  384 int example_set_uc_queue_mapping(int unit, int local_port_id, int incoming_tc, int incoming_dp, int queue)
  385 {
  386   opennsl_error_t rv = OPENNSL_E_NONE;
  387   opennsl_cosq_gport_type_t gport_type;
  388   opennsl_cosq_gport_info_t gport_info;
  389   opennsl_gport_t out_gport;
  390   int queue_id;
  391 
  392   /* Set GPORT according to the given local_port_id */
  393   OPENNSL_GPORT_LOCAL_SET(gport_info.in_gport,local_port_id); 
  394 
  395   gport_type = opennslCosqGportTypeUnicastEgress; 
  396 
  397   rv = opennsl_cosq_gport_handle_get(unit,gport_type,&gport_info);
  398   if (rv != OPENNSL_E_NONE) 
  399   {
  400     printf("Error, in handle get, gport_type $gport_type \n");
  401     return rv;
  402   }
  403 
  404   out_gport = gport_info.out_gport;
  405   rv = _opennsl_petra_egress_queue_from_cosq(unit,&queue_id,queue);
  406   rv = opennsl_cosq_gport_egress_map_set(unit, out_gport, incoming_tc, incoming_dp, queue_id);
  407 
  408   if (rv != OPENNSL_E_NONE) 
  409   {
  410     printf("Error, in uc queue mapping, out_gport $out_gport incoming_tc $incoming_tc incoming_dp $incoming_dp queue $queue \n");
  411   }
  412 
  413   return rv;
  414 }
  415 /* __doxy_func_body_end__ */
  416  
  417 /*  Map incoming MC TC/DP to L/H queue
  418  *  incoming_tc = 0-7
  419  *  incoming_dp = 0-3
  420  *  queue = OPENNSL_COSQ_LOW_PRIORITY
  421  *          OPENNSL_COSQ_HIGH_PRIORITY
  422  */
  423 int example_set_mc_queue_mapping(int unit, int local_port_id, int incoming_tc, int incoming_dp, int queue)
  424 {
  425   opennsl_error_t rv = OPENNSL_E_NONE;
  426   opennsl_cosq_gport_type_t gport_type;
  427   opennsl_cosq_gport_info_t gport_info;
  428   opennsl_gport_t out_gport;
  429   int queue_id;
  430 
  431   /* Set GPORT according to the given local_port_id */
  432   OPENNSL_GPORT_LOCAL_SET(gport_info.in_gport,local_port_id); 
  433 
  434   gport_type = opennslCosqGportTypeLocalPort; 
  435 
  436   rv = opennsl_cosq_gport_handle_get(unit,gport_type,&gport_info);
  437   if (rv != OPENNSL_E_NONE) 
  438   {
  439     printf("Error, in handle get, gport_type $gport_type \n");
  440     return rv;
  441   }
  442 
  443   out_gport = gport_info.out_gport;
  444 
  445   rv = _opennsl_petra_egress_queue_from_cosq(unit,&queue_id,queue);
  446   opennsl_cosq_egress_multicast_config_t  multicast_egress_mapping =  { 0, queue_id,-1,-1};
  447 
  448   rv = opennsl_cosq_gport_egress_multicast_config_set(unit, out_gport, incoming_tc, incoming_dp, 
  449               OPENNSL_COSQ_MULTICAST_SCHEDULED, 
  450               &multicast_egress_mapping );
  451 
  452   if (rv != OPENNSL_E_NONE) 
  453   {
  454     printf("Error, in mc queue mapping, out_gport $out_gport incoming_tc $incoming_tc incoming_dp $incoming_dp queue $queue \n");
  455   }
  456 
  457   return rv;
  458 }
  459 /* __doxy_func_body_end__ */
  460 
  461 /* Setup MAC forwading
  462  * dest_type = 0 for Local Port
  463  *             1 for MC Group
  464  */
  465 int example_setup_mac_forwarding(int unit, opennsl_mac_t mac, opennsl_vlan_t vlan, int dest_type, int dest_id)
  466 {
  467   opennsl_error_t rv = OPENNSL_E_NONE;
  468   opennsl_gport_t dest_gport;
  469   opennsl_l2_addr_t l2_addr;
  470 
  471   opennsl_l2_addr_t_init(&l2_addr, mac, vlan);   
  472   /* Create MC or PORT address forwarding */
  473   if(dest_type == 0) 
  474   {
  475     l2_addr.flags = OPENNSL_L2_STATIC;
  476     OPENNSL_GPORT_LOCAL_SET(dest_gport, dest_id);
  477     l2_addr.port = dest_gport;
  478   } 
  479   else 
  480   {
  481     l2_addr.flags = OPENNSL_L2_STATIC | OPENNSL_L2_MCAST;
  482     l2_addr.l2mc_group = dest_id;
  483   }
  484 
  485   rv = opennsl_l2_addr_add(unit,&l2_addr);
  486   if (rv != OPENNSL_E_NONE) 
  487   {
  488     printf("Error, in example_setup_mac_forwarding, dest_type $dest_type dest_id $dest_id \n");
  489   }
  490 
  491   return rv;
  492 }
  493 /* __doxy_func_body_end__ */
  494 
  495 /*  Main function
  496  *  opennsl_local_port_id = The desiganted port we want to configure
  497  *  is_tm = 0 for PP Ports
  498  *          1 for TM ports
  499  * 
  500  */
  501 int example_egress_transmit_application(int unit, int opennsl_local_port_id, int is_tm)
  502 {
  503   opennsl_error_t rv = OPENNSL_E_NONE;
  504   int tc = 0;
  505   int dp = 0;
  506   int my_modid = 0;
  507 
  508   /* Init */
  509   printf("Starting Egress Transmit Application\n");
  510   rv = opennsl_stk_modid_get(unit, &my_modid);
  511   if (rv != OPENNSL_E_NONE) 
  512   {
  513     printf("opennsl_stk_my_modid_get failed $rv\n");
  514     return rv;
  515   }
  516 
  517   /* Create MC group */
  518   printf("Setting up Multicast Groups.\n");
  519   int mc_group_id = 5005;
  520   int cud = 0;
  521   opennsl_multicast_t mc_group = mc_group_id;
  522 
  523   /* We want to overwrite any existing group */
  524   opennsl_multicast_destroy(unit, mc_group);
  525 
  526   /* Open Egress MC with the designated port as destination and 0 as cud */
  527   rv = multicast__open_egress_mc_group_with_local_ports(unit, mc_group_id, &opennsl_local_port_id, &cud, 1, 0);
  528   if (rv != OPENNSL_E_NONE) return rv;
  529 
  530   /* Forwarding Setup, only relevant for PP ports */
  531   if(is_tm == 0) 
  532   {
  533     printf("Setting up MAC Forwarding for PP Ports.\n");
  534     opennsl_mac_t incoming_mac_uc;
  535     opennsl_mac_t incoming_mac_mc;
  536     incoming_mac_uc[5] = 0x1;
  537     int incoming_vlan_uc = 1;
  538     incoming_mac_mc[5] = 0x2;
  539     int incoming_vlan_mc = 1;
  540 
  541     rv = example_setup_mac_forwarding(unit, incoming_mac_uc, incoming_vlan_uc, 0, opennsl_local_port_id);
  542     if (rv != OPENNSL_E_NONE) return rv;
  543     rv = example_setup_mac_forwarding(unit, incoming_mac_mc, incoming_vlan_mc, 1, mc_group_id);
  544     if (rv != OPENNSL_E_NONE) return rv;
  545   }
  546 
  547   /* Set OFP Bandwidth to 5G */
  548   printf("Setting Port Bandwidth.\n");
  549   int max_bandwidth = 5000000; 
  550   rv = example_set_ofp_bandwidth(unit, opennsl_local_port_id, max_bandwidth);
  551   if (rv != OPENNSL_E_NONE) return rv;
  552 
  553   /* Set Weight for the Low Priority Queues */
  554   printf("Setting Weight for low priority queues.\n");
  555   int uc_weight =1;
  556   int mc_weight = 2;
  557   rv = example_set_weight(unit, opennsl_local_port_id, OPENNSL_COSQ_LOW_PRIORITY, 0, uc_weight); 
  558   if (rv != OPENNSL_E_NONE) return rv;
  559   rv = example_set_weight(unit, opennsl_local_port_id, OPENNSL_COSQ_LOW_PRIORITY, 1, mc_weight); 
  560   if (rv != OPENNSL_E_NONE) return rv;
  561 
  562   /* Set UC over MC for the High Priority Queues */
  563   printf("Setting Strict Priority for high priority queues.\n");
  564   rv = example_set_sp(unit,opennsl_local_port_id, OPENNSL_COSQ_HIGH_PRIORITY, 0);
  565   if (rv != OPENNSL_E_NONE) return rv;
  566 
  567   /* Set UC Queue Mapping */
  568   printf("Setting UC queue mapping.\n");
  569   for (tc=0; tc <= 3 ; tc++) 
  570   { 
  571     for(dp = 0; dp <=3; dp++ ) 
  572     {
  573       /* printf("\t TC $tc DP $dp --> Low Priority.\n"); */
  574       rv = example_set_uc_queue_mapping(unit, opennsl_local_port_id, tc, dp, OPENNSL_COSQ_LOW_PRIORITY);
  575       if (rv != OPENNSL_E_NONE) return rv;
  576     }
  577   }
  578 
  579   for (tc=4; tc <= 7 ; tc++) 
  580   {
  581     for(dp = 0; dp <=3; dp++ ) 
  582     {
  583       /* printf("\t TC $tc DP $dp --> High Priority.\n"); */
  584       rv = example_set_uc_queue_mapping(unit, opennsl_local_port_id, tc, dp, OPENNSL_COSQ_HIGH_PRIORITY);
  585       if (rv != OPENNSL_E_NONE) return rv;
  586     }
  587   }
  588 
  589   /* Set MC Queue Mapping */
  590   printf("Setting MC queue mapping.\n");
  591   for (tc=0; tc <= 5 ; tc++) 
  592   {
  593     for(dp = 0; dp <=3; dp++ ) 
  594     {
  595       /*printf("\t TC $tc DP $dp --> Low Priority.\n"); */
  596       rv = example_set_mc_queue_mapping(unit, opennsl_local_port_id, tc, dp, OPENNSL_COSQ_LOW_PRIORITY);
  597       if (rv != OPENNSL_E_NONE) return rv;
  598     }
  599   }
  600 
  601   for (tc=6; tc <= 7 ; tc++) 
  602   {
  603     for(dp = 0; dp <=3; dp++ ) 
  604     {
  605       /* printf("\t TC $tc DP $dp --> High Priority.\n"); */
  606       rv = example_set_mc_queue_mapping(unit, opennsl_local_port_id, tc, dp, OPENNSL_COSQ_HIGH_PRIORITY);
  607       if (rv != OPENNSL_E_NONE) return rv;
  608     }
  609   }
  610 
  611   printf("Engress Transmit Application Completed Successfully.\n");
  612 
  613   return rv;
  614 }
  615 /* __doxy_func_body_end__ */
  616 
  617 /*****************************************************************/
  624 int main(int argc, char *argv[])
  625 {
  626   int rv = 0;
  627   int unit = DEFAULT_UNIT;
  628   int choice;
  629   int outport;
  630 
  631   if((argc != 1) || ((argc > 1) && (strcmp(argv[1], "--help") == 0))) 
  632   {
  633     printf("%s\n\r", example_usage);
  634     return OPENNSL_E_PARAM;
  635   }
  636 
  637   /* Initialize the system. */
  638   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  639   if(rv != OPENNSL_E_NONE) 
  640   {
  641     printf("\r\nFailed to initialize the system. Error: %s\r\n",
  642            opennsl_errmsg(rv));
  643     return rv;
  644   }
  645 
  646   /* cold boot initialization commands */
  647   rv = example_port_default_config(unit);
  648   if (rv != OPENNSL_E_NONE) 
  649   {
  650     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  651            rv, opennsl_errmsg(rv));
  652   }
  653 
  654   /* Add ports to default vlan. */
  655   printf("Adding ports to default vlan.\r\n");
  656   rv = example_switch_default_vlan_config(unit);
  657   if(rv != OPENNSL_E_NONE) 
  658   {
  659     printf("\r\nFailed to add default ports. Error: %s\r\n",
  660         opennsl_errmsg(rv));
  661   }
  662 
  663   while(1) {
  664     printf("\r\nUser menu: Select one of the following options\r\n");
  665     printf("1. Apply CoS mapping to the traffic\n");
  666 #ifdef INCLUDE_DIAG_SHELL
  667     printf("9. Launch diagnostic shell\n");
  668 #endif
  669     printf("0. Quit the application.\r\n");
  670 
  671     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  672     {
  673       printf("Invalid option entered. Please re-enter.\n");
  674       continue;
  675     }
  676     switch(choice)
  677     {
  678       case 1:
  679       {
  680         printf("Enter the egress port number(belonging to core-0):\n");
  681         if(example_read_user_choice(&outport) != OPENNSL_E_NONE)
  682         {
  683           printf("Invalid value entered. Please re-enter.\n");
  684           continue;
  685         }
  686 
  687         /* Create CoS mapping to test with traffic */
  688         rv = example_egress_transmit_application(0, outport, 0);
  689         if (rv != OPENNSL_E_NONE) 
  690         {
  691           printf("\r\nFailed to create the CoS mapping. Error: %s\r\n",
  692               opennsl_errmsg(rv));
  693         }  
  694 
  695         break;
  696       }
  697 
  698 #ifdef INCLUDE_DIAG_SHELL
  699       case 9:
  700       {
  701         opennsl_driver_shell();
  702         break;
  703       }
  704 #endif
  705 
  706       case 0:
  707       {
  708         printf("Exiting the application.\n");
  709         rv = opennsl_driver_exit();
  710         return rv;
  711       }
  712       default:
  713       break;
  714     } /* End of switch */
  715   } /* End of while */
  716 
  717   return rv;
  718 }
  719 /* __doxy_func_body_end__ */
