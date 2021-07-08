#NSDI 2020 

#A simple Explicit Congestion Control for Wireless Networks

##Environment:Wireless Networks; 

##Method:Explicit Congestion Control

###Challenge 1:Wireless Networks mean Link Rate with time-varying. Main behaviour is current rate will havle or double in next RTT.

###direction 1: In order to settle down challanges, we will discuss some common methods.first,CCA based on loss tends to fill up the second buffer in any position of network,which will lead to large queuing delays,especially this behavior will impact larger at celluar network,which uses large buffer to avoid packet loss;some CCA based on RTT or send/receive rate have an improvement over loss-based schemes,but they are not ok for time-varying environment;even some CCA rely on parameters, inluding Sprout and Verus,and direct to worse results;finally,XCP and RCP based on Explicit methods can reflect link rate to senders in short time and even adjsut the variance at a large range.
 
###Focus:Track time-varying wireless link rates accurately.

###Challenge 2:Deployment challenges for explicit control.

###direction 2:(1)Some explicit control require major changes to packet headers,routers,and endpoints. they have to use new packet fields to carry multi-bit feedback information. But these informations is difficult to add into packet fields.for instance,IP options often are drop in many wide-area routers;TCP options need to negotiate contents with midddlebox and IPSec encryption,new fields are prone to create problems;(2) scheme needs co-existence with all things before method proposes. I call this scheme as evolvedCCA.

###Focus：No modification to packet headers and evovled.

ABC provides some informations to rethink no modification about explicit control. 
**Exploiting ECN** 
