#include <linux/random.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <net/tcp.h>

// #include "tcp_cong.c"

// /* Two methods of hybrid slow start */
// #define HYSTART_ACK_TRAIN	0x1
// #define HYSTART_DELAY		0x2
// #define HYSTART_MIN_SAMPLES	8
// #define HYSTART_DELAY_MIN	(4000U)	/* 4 ms */
// #define HYSTART_DELAY_MAX	(16000U)	/* 16 ms */
// #define HYSTART_DELAY_THRESH(x)	clamp(x, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)


// static int initial_ssthresh __read_mostly;
// static int hystart __read_mostly = 1;
// static int hystart_detect __read_mostly = HYSTART_ACK_TRAIN | HYSTART_DELAY;
// static int hystart_low_window __read_mostly = 16;
// static int hystart_ack_delta_us __read_mostly = 2000;

// module_param(initial_ssthresh, int, 0644);
// MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
// module_param(hystart, int, 0644);
// MODULE_PARM_DESC(hystart, "turn on/off hybrid slow start algorithm");
// module_param(hystart_detect, int, 0644);
// MODULE_PARM_DESC(hystart_detect, "hybrid slow start detection mechanisms"
// 		 " 1: packet-train 2: delay 3: both packet-train and delay");
// module_param(hystart_low_window, int, 0644);
// MODULE_PARM_DESC(hystart_low_window, "lower bound cwnd for hybrid slow start");
// module_param(hystart_ack_delta_us, int, 0644);
// MODULE_PARM_DESC(hystart_ack_delta_us, "spacing between ack's indicating train (usecs)");
/* Probing changes rate by 5% up and down of current rate. */
static const u32 pcc_probing_eps = 5;
static const u32 pcc_probing_eps_part = 100;
static const u64 pcc_factor = 1000; /* scale for fractions, utilities, gradients, ... */

static const u64 pcc_min_rate = 1024u;
static const u32 pcc_min_rate_packets_per_rtt = 2;
static const u32 pcc_interval_per_packet = 50;
static const u32 pcc_alpha = 100;

static const u32 pcc_grad_step_size = 25; /* defaults step size for gradient ascent */
static const u32 pcc_max_swing_buffer = 2; /* number of RTTs to dampen gradient ascent */

static const u32 pcc_lat_infl_filter = 30; /* latency inflation below 3% is ignored */

/* Rates must differ by at least 2% or gradients are very noisy. */
static const u32 pcc_min_rate_diff_ratio_for_grad = 20;

static const u32 pcc_min_change_bound = 100; /* first rate change is at most 10% of rate */
static const u32 pcc_change_bound_step = 70; /* consecutive rate changes can go up by 7% */
static const u32 pcc_min_amp = 2; /* starting amplifier for gradient ascent step size */

/* The number of past monitor intervals used for decision making.*/
static const u32 pcc_intervals = 4;

static s64 add_cwnd = 0;

#define USE_PROBING

enum PCC_DECISION {
    PCC_RATE_UP,
    PCC_RATE_DOWN,
    PCC_RATE_STAY
}; 

struct pcc_interval{
    u64 rate;		/* sending rate of this interval, bytes/sec */
	
	s64 recv_start; /* timestamps for when interval was waiting for acks */
	s64 recv_end;

	s64 send_start; /* timestamps for when interval data was being sent */
	s64 send_end;

	s64 start_rtt; /* smoothed RTT at start and end of this interval */
	s64 end_rtt;

	u32 packets_sent_base; /* packets sent before this interval started */
	u32 packets_ended;

	s64 utility; /* observed utility of this interval */
	u32 lost; /* packets sent during this interval that were lost */
	u32 delivered; /* packets sent during this interval that were delivered */

};

static int  id  =   0;

struct ic {
    struct pcc_interval *intervals; /* containts stats for 1 RTT */
    // u32	cnt;		/* increase cwnd by 1 after ACKs */
	// u32	last_max_cwnd;	/* last maximum snd_cwnd */
	// u32	last_cwnd;	/* the last snd_cwnd */
	// u32	last_time;	/* time when updated last_cwnd */
	// u32	bic_origin_point;/* origin point of bic function */
	// u32	bic_K;		/* time to origin point
	// 			   from the beginning of the current epoch */
	// u32	delay_min;	/* min delay (usec) */
	// u32	epoch_start;	/* beginning of an epoch */
	// u32	ack_cnt;	/* number of acks */
	// u32	tcp_cwnd;	/* estimated tcp cwnd */
	// u16	unused;
	// u8	sample_cnt;	/* number of samples to decide curr_rtt */
	// u8	found;		/* the exit point is found? */
	// u32	round_start;	/* beginning of each round */
	// u32	end_seq;	/* end_seq of the round */
	// u32	last_ack;	/* last time when the ACK spacing is close */
	// u32	curr_rtt;	/* the minimum rtt of current round */
    // struct bictcp cubic;

    u64 temp_cwnd;
	int send_index; /* index of interval currently being sent */
	int recive_index; /* index of interval currently receiving acks */

	s64 rate; /* current sending rate */
	s64 last_rate; /* previous sending rate */

	/* utility function pointer (can be loss- or latency-based) */
	void (*util_func)(struct ic *, struct pcc_interval *, struct sock *);

	bool start_mode;
	bool moving; /* using gradient ascent to move to a new rate? */
	bool loss_state;

	bool wait;

	enum PCC_DECISION last_decision; /* most recent rate change direction */
	u32 lost_base; /* previously lost packets */
	u32 delivered_base; /* previously delivered packets */

	// debug helpers
	int id;
	int decisions_count;

	u32 packets_sent;
	u32 packets_counted;
	u32 spare;

	s32 amplifier; /* multiplier on the current step size */
	s32 swing_buffer; /* number of RTTs left before step size can grow */
	s32 change_bound; /* maximum change as a proportion of the current rate */
};

// static inline void ic_bictcp_reset(struct ic *ca){
//     ca->cnt =0;
//     ca->last_max_cwnd=0;
//     ca->last_cwnd=0;
//     ca->last_time=0;
//     ca->bic_origin_point=0;
//     ca->bic_K=0;
//     ca->delay_min=0;
//     ca->epoch_start=0;
//     ca->ack_cnt=0;
//     ca->tcp_cwnd =0;
//     ca->found=0;
// }

static inline u32 clock_us(const struct sock *sk)
{
	//tcp_sk(sk) means struct tcp_sock *tp  
	//tcp_mstamp means most recent package received/sent
	return tcp_sk(sk)->tcp_mstamp;
}


static u32 pcc_get_rtt(struct tcp_sock *tp){
    if(tp->srtt_us){
        return max(tp->srtt_us>>3,1U);
    }else{
        return USEC_PER_MSEC;
    }
}

// static inline void bictcp_hystart_reset(struct sock *sk)
// {
//     struct tcp_sock *tp =tcp_sk(sk);
//     struct ic *ca =inet_csk_ca(sk);
//     ca->round_start =ca->last_ack =clock_us(sk);
//     ca->end_seq =tp->snd_nxt;
//     ca->curr_rtt =~0U;
//     ca->sample_cnt =0;

// }

bool pcc_valid(struct ic *ca){
    return (ca && ca->intervals && ca->intervals[0].rate);
}

static void pcc_setup_intervals_probing(struct ic *ca)
{
    //setting all probing intervals' velocity countting on ca->rate 
	u64 rate_low, rate_high;
	char rand;
	int i;

	get_random_bytes(&rand, 1);
	rate_high = ca->rate * (pcc_probing_eps_part + pcc_probing_eps);
	rate_low = ca->rate * (pcc_probing_eps_part - pcc_probing_eps);

	rate_high /= pcc_probing_eps_part;
	rate_low /= pcc_probing_eps_part;

	for (i = 0; i < pcc_intervals; i += 2) {
		if ((rand >> (i / 2)) & 1) {
			ca->intervals[i].rate = rate_low;
			ca->intervals[i + 1].rate = rate_high;
		} else {
			ca->intervals[i].rate = rate_high;
			ca->intervals[i + 1].rate = rate_low;
		}

		ca->intervals[i].packets_sent_base = 0;
		ca->intervals[i + 1].packets_sent_base = 0;
	}

	ca->send_index = 0;
	ca->recive_index = 0;
	ca->wait = false;
}
//setting current interval velocity
static void pcc_setup_intervals_moving(struct ic *ca)
{
	ca->intervals[0].packets_sent_base = 0;
	ca->intervals[0].rate = ca->rate;
	ca->send_index = 0;
	ca->recive_index = 0;
	ca->wait = false;
}

static void start_interval(struct sock *sk, struct ic *ca)
{
	u64 rate = ca->rate;
	struct pcc_interval *interval;

	if (!ca->wait) {
		interval = &ca->intervals[ca->send_index];
		interval->packets_ended = 0;
		interval->lost = 0;
		interval->delivered = 0;
		interval->packets_sent_base = tcp_sk(sk)->data_segs_out;
		interval->packets_sent_base = max(interval->packets_sent_base, 1U);
		interval->send_start = clock_us(sk);
		rate = interval->rate;
	}
    //interval->rate
	rate = max(rate, pcc_min_rate);
	rate = min(rate, sk->sk_max_pacing_rate);
	sk->sk_pacing_rate = rate;
    // pcc_convert_pacing_rate(sk);
    // //?????????????????????????????????
    // sk->sk_pacing_rate =max(sk->sk_pacing_rate , rate);
}

static s64 pcc_calc_util_grad(s64 rate_1,s64 util_1 , s64 rate_2, s64 util_2){
    s64 rate_diff_ratio =(pcc_factor *(rate_2-rate_1))/rate_1;
    if(rate_diff_ratio <pcc_min_rate_diff_ratio_for_grad && rate_diff_ratio>-1 * pcc_min_rate_diff_ratio_for_grad)
    return 0;

    return (pcc_factor*pcc_factor*(util_2-util_1))/(rate_2-rate_1); 
}

static void pcc_calc_utility_vivace_latency(struct ic *ca,struct pcc_interval *interval,struct sock *sk){
    s64 loss_ratio,delivered,lost,mss,rate,throughput,util;
    s64 lat_infl =0;
    s64 rtt_diff ;
    s64 rtt_diff_thresh = 0;
	s64 send_dur = interval->send_end - interval->send_start;
	s64 recv_dur = interval->recv_end - interval->recv_start;

    lost = interval->lost;
    delivered = interval->delivered;
    mss =tcp_sk(sk)->mss_cache;
    //????????????rate???
    rate = interval->rate;
    throughput =0;
    if(recv_dur>0)
    //????????????????????????
    throughput= (USEC_PER_SEC *delivered *mss)/recv_dur;

    if(delivered ==0){
     printk(KERN_INFO "No packets delivered\n");
     interval->utility =0;
     return;
    }
    //throughput> 0?????????
    rtt_diff =interval->end_rtt - interval->start_rtt;
    if(throughput >0)
    //???????????????????????????
    rtt_diff_thresh = (2*USEC_PER_SEC *mss)/ throughput;
    if(send_dur>0)
    lat_infl =(pcc_factor *rtt_diff)/send_dur;

    printk(KERN_INFO"%d ucalc: lat (%lld->%lld) lat_infl %lld\n",ca->id, interval->start_rtt / USEC_PER_MSEC, interval->end_rtt / USEC_PER_MSEC,lat_infl);
    
    //?????????????????????????????????????????????????????????????????????
    if(rtt_diff <rtt_diff_thresh&&rtt_diff>-1*rtt_diff_thresh)
    lat_infl =0;

    if(lat_infl<pcc_lat_infl_filter &&lat_infl>-1*pcc_lat_infl_filter)
    lat_infl =0;

    if(lat_infl<0 &&ca->start_mode)
    lat_infl =0; 

    loss_ratio =(lost *pcc_factor)/(lost+delivered);
    //?????????????????????????????????????????????????????????????????????
    if(ca->start_mode &&loss_ratio <100)
    loss_ratio =0;

    util = /* int_sqrt((u64)rate)*/ rate - (rate * (900 * lat_infl + 11 * loss_ratio)) / pcc_factor;

    printk(KERN_INFO
		"%d ucalc: rate %lld sent %u delv %lld lost %lld lat (%lld->%lld) util %lld rate %lld thpt %lld\n",
		 ca->id, rate, interval->packets_ended - interval->packets_sent_base,
		 delivered, lost, interval->start_rtt / USEC_PER_MSEC, interval->end_rtt / USEC_PER_MSEC, util, rate, throughput);
	interval->utility = util;

}

static void pcc_convert_pacing_rate(struct ic *ca,struct sock *sk){
    u64 pacing_rate;
    struct tcp_sock *tp =tcp_sk(sk);
    if(tp->snd_cwnd >= tp->snd_cwnd_clamp){
        tp->snd_cwnd = tp->snd_cwnd_clamp;
    }
    //????????????
    if(tp->snd_cwnd<=4ULL)
    tp->snd_cwnd =4ULL;

    pacing_rate =tp->snd_cwnd;
    pacing_rate /=pcc_get_rtt(tcp_sk(sk));
    //pacing_rate /= pcc_get_rtt(tp);
    pacing_rate *=tp->mss_cache;

    pacing_rate =max(pcc_min_rate*512 ,pacing_rate);
    ca->rate =pacing_rate;

}
static enum PCC_DECISION
pcc_get_decision(struct ic *ca,u32 new_rate){

    if(ca->rate ==new_rate)
        return PCC_RATE_STAY;

    return ca->rate <new_rate ?PCC_RATE_UP :PCC_RATE_DOWN;
}

static u32 pcc_decide_rate(struct ic *ca)
{
    //??????????????????????????????????????????????????????????????????
	bool run_1_res, run_2_res, did_agree;

	run_1_res = ca->intervals[0].utility > ca->intervals[1].utility;
	run_2_res = ca->intervals[2].utility > ca->intervals[3].utility;

	//?????? ?????? ??????????????????
	did_agree = !((run_1_res == run_2_res) ^ 
			(ca->intervals[0].rate == ca->intervals[2].rate));

	if (did_agree) {
		if (run_2_res) {
			ca->last_rate = ca->intervals[2].rate;
			ca->intervals[0].utility = ca->intervals[2].utility;
		} else {
			ca->last_rate = ca->intervals[3].rate;
			ca->intervals[0].utility = ca->intervals[3].utility;
		}
		return run_2_res ? ca->intervals[2].rate :
				   ca->intervals[3].rate;
	} else {
		return ca->rate;
	}
}

static void pcc_decide(struct sock *sk,struct ic *ca)
{
    struct tcp_sock *tp =tcp_sk(sk);
    struct pcc_interval *interval;
    u64 new_rate;
    int index;

    for(index=0;index<pcc_intervals;++index){
        interval= &ca->intervals[index];
        (*ca->util_func)(ca,interval,sk);
    }

    //??????pcc_rate??????new_rate

    new_rate=pcc_decide_rate(ca);

    if(new_rate != ca->rate){
        printk(KERN_INFO "%d decide: on new rate %d %d (%d)\n", ca->id, ca->rate <new_rate,new_rate, ca->decisions_count);
        ca->moving =true;
        pcc_setup_intervals_moving(ca);
    }
    else{
        printk(KERN_INFO "%d decide:stay %d (%d)\n",ca->id,ca->rate,ca->decisions_count);
        pcc_setup_intervals_probing(ca);
    }
    // //?????????????????? 1. ??????????????? 2.????????????????????????
    // add_cwnd = (new_rate -ca->rate) *pcc_get_rtt(tp); 
    // add_cwnd /= tp->mss_cache;
    // add_cwnd /= USEC_PER_SEC;      //?????? * RTT???
    // tp->snd_cwnd +=add_cwnd;
    // //???????????????
    // if(tp->snd_cwnd <=4ULL){
    //     tp->snd_cwnd =4ULL;
    // }

    // pcc_convert_pacing_rate(ca,sk);

    ca->rate =new_rate;
    
    start_interval(sk,ca);
    ++ca->decisions_count;
}

static void pcc_update_step(struct ic *ca,s64 step){
    if((step>0) ==(ca->rate >ca->last_rate)){
        if(ca->swing_buffer > 0)
            ca->swing_buffer--;
        else
        {
            /* code */
            ca->amplifier++;
        }
        
    }
    else{
        ca->swing_buffer = min(ca->swing_buffer+1,pcc_max_swing_buffer);
        ca->amplifier = pcc_min_amp;//??????
        ca->change_bound =pcc_min_change_bound;
    }

}

static s64 pcc_apply_change_bound(struct ic *ca,s64 step)
{
    s32 step_sign;
	s64 change_ratio;
	if (ca->rate == 0)
		return step;

	step_sign = step > 0 ? 1 : -1;
	step *= step_sign;
	change_ratio = (pcc_factor * step) / ca->rate;

	if (change_ratio > ca->change_bound) {
		step = (ca->rate * ca->change_bound) / pcc_factor;
		printk("bound %u rate %u step %lld\n", ca->change_bound, ca->rate, step);
		ca->change_bound += pcc_change_bound_step;
	} else {
		ca->change_bound = pcc_min_change_bound;
	}
	return step_sign * step;

}


static u32 pcc_decide_rate_moving(struct sock *sk, struct ic *ca)
{
    struct tcp_sock *tp =tcp_sk(sk);
	struct pcc_interval *interval = &ca->intervals[0];
	s64 utility, prev_utility;
	s64 grad, step, min_step;

	prev_utility = interval->utility;
	(*ca->util_func)(ca, interval, sk);
	utility = interval->utility;
	
	printk(KERN_INFO "%d mv: pr %u pu %lld nr %u nu %lld\n",
		   ca->id, ca->last_rate, prev_utility, ca->rate, utility);

	grad = pcc_calc_util_grad(ca->rate, utility, ca->last_rate, prev_utility);

	step = grad * pcc_grad_step_size; /* gradient ascent */
	pcc_update_step(ca, step); /* may accelerate/decellerate changes */
	step *= ca->amplifier; 
    step /= pcc_factor;
	step = pcc_apply_change_bound(ca, step);

	
	min_step = (ca->rate * pcc_min_rate_diff_ratio_for_grad) / pcc_factor;
	min_step *= 11; /* step slightly larger than the minimum */
	min_step /= 10;
	if (step >= 0 && step < min_step)
		step = min_step;
	else if (step < 0 && step > -1 * min_step)
		step = -1 * min_step;
    
     //?????????????????? 1. ??????????????? 2.????????????????????????
    add_cwnd = step *pcc_get_rtt(tcp_sk(sk)); 
    add_cwnd /= tp->mss_cache;
    add_cwnd /= USEC_PER_SEC;      //?????? * RTT???
    tp->snd_cwnd +=add_cwnd;
    
    pcc_convert_pacing_rate(ca,sk);

	printk(KERN_INFO "%d mv: grad %lld step %lld amp %d min_step %lld\n",
		   ca->id, grad, step, ca->amplifier, min_step);

	return ca->rate;
}

static void pcc_decide_moving(struct sock *sk, struct ic *ca)
{
    //??????????????????????????????????????????????????????????????????grad????????????????????????????????????????????????
	s64 new_rate = pcc_decide_rate_moving(sk, ca);
	enum PCC_DECISION decision = pcc_get_decision(ca, new_rate);
	enum PCC_DECISION last_decision = ca->last_decision;
    //??????????????????????????????????????????????????????
    s64 packet_min_rate = (USEC_PER_SEC * pcc_min_rate_packets_per_rtt *
        tcp_sk(sk)->mss_cache) / pcc_get_rtt(tcp_sk(sk));
    new_rate = max(new_rate, packet_min_rate);
	ca->last_rate = ca->rate;
	printk(KERN_INFO "%d moving: new rate %lld (%d) old rate %d\n",
		   ca->id, new_rate,
		   ca->decisions_count, ca->last_rate);
	ca->rate = new_rate;
	if (decision != last_decision) {

#ifdef USE_PROBING
		ca->moving = false;
		pcc_setup_intervals_probing(ca);
#else
		pcc_setup_intervals_moving(ca);
#endif
	} else {
		pcc_setup_intervals_moving(ca);
	}

	start_interval(sk, ca);
}

// slow start phase
static void pcc_decide_slow_start(struct sock *sk, struct ic *ca)
{
    //?????????????????????pcc->rate ??????
    struct tcp_sock *tp = tcp_sk(sk);
	struct pcc_interval *interval = &ca->intervals[0];
	s64 utility, prev_utility, adjust_utility, prev_adjust_utility, tmp_rate;
	// s64 tmp_rate;
    u64 extra_rate;

	prev_utility = interval->utility;
	(*ca->util_func)(ca, interval, sk);
	utility = interval->utility;

	/* The new utiltiy should be at least 75% of the expected utility given
	 * a significant increase. If the utility isn't as high as expected, then
	 * we end slow start.
	 */
	adjust_utility = utility * (utility > 0 ? 1000 : 750) / ca->rate;
	prev_adjust_utility = prev_utility * (prev_utility > 0 ? 750 : 1000) /
				ca->last_rate;

	printk(KERN_INFO "%d: start mode: r %lld u %lld pr %lld pu %lld\n",
		ca->id, ca->rate, utility, ca->last_rate, prev_utility);
	//if (utility > prev_utility) {
	if (adjust_utility > prev_adjust_utility) {
        //??????????????????rate??????
		ca->last_rate = ca->rate;
        //?????????????????????double target rate
        //???????????????????????????????????????????????????
		extra_rate = ca->intervals[0].delivered *
				 tcp_sk(sk)->mss_cache;
		extra_rate = min(extra_rate, ca->rate / 2);

		ca->rate += extra_rate; //extra_rate;

         //??????????????????????????????
        add_cwnd=extra_rate*pcc_get_rtt(tcp_sk(sk))/tp->mss_cache/USEC_PER_SEC;
        tp->snd_cwnd +=add_cwnd;

        pcc_convert_pacing_rate(ca,sk);

		interval->utility = utility;
		interval->rate = ca->rate;
		ca->send_index = 0;
		ca->recive_index = 0;
		ca->wait = false;
	} else {
        //??????????????????????????????????????????????????????start mode
		tmp_rate = ca->last_rate;
		ca->last_rate = ca->rate;
		ca->rate = tmp_rate;
		ca->start_mode = false;
		printk(KERN_INFO "%d: start mode ended\n", ca->id);

#ifdef USE_PROBING
        pcc_setup_intervals_probing(ca);
#else
		ca->moving = true;
		pcc_setup_intervals_moving(ca);
#endif
    }

	start_interval(sk, ca);
}

bool send_interval_ended(struct pcc_interval *interval, struct tcp_sock *tsk, struct ic *ca){
    int packets_sent = tsk->data_segs_out - interval->packets_sent_base;
    
    if(packets_sent < pcc_interval_per_packet)
    return false;
    //??????interval???????????????
    //??????????????????????????????????????????
    if (ca->packets_counted>interval->packets_sent_base)
    {
        interval->packets_ended = tsk->data_segs_out;
        return true;
        /* code */
    }
    return false;

}

bool recive_interval_ended(struct pcc_interval *interval, struct tcp_sock *tsk, struct ic *ca){
    //?????????????????????
    return interval->packets_ended &&interval->packets_ended - 10 < ca->packets_counted;

}

// ????????????????????????????????????????????????????????????????????????????????????????????????ack
static void start_next_send_interval(struct sock *sk,struct ic *ca){
    ++ca->send_index;
    //start_mode??????????????????
    //pcc->send_index ==pcc_intervals
    if(ca->send_index ==pcc_intervals || ca->start_mode || ca->moving){
        ca->wait =true;
    }

    start_interval(sk, ca);

}

static void
pcc_update_interval(struct pcc_interval *interval,	struct ic *ca,
		struct sock *sk)
{
	interval->recv_end = clock_us(sk);
	interval->end_rtt = tcp_sk(sk)->srtt_us >> 3;
	if (interval->lost + interval->delivered == 0) {
		interval->recv_start = clock_us(sk);
		interval->start_rtt = tcp_sk(sk)->srtt_us >> 3;
	}

	interval->lost += tcp_sk(sk)->lost - ca->lost_base;
	interval->delivered += tcp_sk(sk)->delivered - ca->delivered_base;
}

/* Updates the PCC model */
static void pcc_process(struct ic *ca,struct sock *sk){
    struct tcp_sock *tsk=tcp_sk(sk);
    struct pcc_interval *interval;
    int index;
    u32 before;
    if(!pcc_valid(ca)){
        return;
    }
    //????????????,????????????
    if(ca->loss_state) 
    goto end;

    if(!ca->wait){
        interval =&ca->intervals[ca->send_index];
        if(send_interval_ended(interval,tsk,ca)){
            interval->send_end =clock_us(sk);
            //??????
            start_next_send_interval(sk,ca);
        }
    }

    index =ca->recive_index;
    interval =&ca->intervals[index];

    before =ca->packets_counted;
    ca->packets_counted =tsk->delivered +tsk->lost-ca->spare;

    if(!interval->packets_sent_base)
    goto end;

    // ????????????counted > =sent ,???????????????interval
    if(before > 10+ interval->packets_sent_base){
        pcc_update_interval(interval,ca,sk);

    }

      //???  ??????????????????????????????
    if(recive_interval_ended(interval,tsk,ca)){
        ++ca->recive_index;
        if(ca->start_mode){
            // slow_start_phase: Double target rate
            pcc_decide_slow_start(sk,ca);
        }

        else if(ca->moving)
            pcc_decide_moving(sk,ca);

        //pcc->recive_index ==pcc_intervals

        else if(ca->recive_index == pcc_intervals)
            pcc_decide(sk,ca);

    }

    end:
        ca->lost_base =tsk->lost;
        ca->delivered_base =tsk->delivered;
}


// static inline void ic_pcc_reset(struct sock *sk){
//     struct ic *ca=inet_csk_ca(sk);
//     ++id;
//     ca->id =id;
//     ca->amplifier =pcc_min_amp;
//     ca->swing_buffer=0;
//     ca->change_bound=pcc_min_change_bound;
//     ca->rate =pcc_min_rate*512;
//     ca->last_rate =pcc_min_rate*512;
//     ca->start_mode =true;
//     ca->moving=false;
//     ca->intervals[0].utility=S64_MIN;
//     ca->util_func=&pcc_calc_utility_vivace_latency;

//     pcc_setup_intervals_probing(ca);
//     start_interval(sk,ca);
//     cmpxchg(&sk->sk_pacing_status,SK_PACING_NONE,SK_PACING_NEEDED);
// }

// static inline void bictcp_update(struct bictcp *ca, u32 cwnd, u32 acked){
//     u32 delta, bic_target, max_cnt;
// 	u64 offs, t;

//     ca->ack_cnt += acked;	/* count the number of ACKed packets */
//     if (ca->last_cwnd == cwnd &&
// 	    (s32)(tcp_jiffies32 - ca->last_time) <= HZ / 32)
// 		return;
    

// }
static void ic_init(struct sock *sk){
    struct ic *ca =inet_csk_ca(sk);
    // ic_bictcp_reset(ca);
    // if(hystart)
    // bictcp_hystart_reset(sk);
    // if (!hystart && initial_ssthresh)
	// 	tcp_sk(sk)->snd_ssthresh = initial_ssthresh;

    ca->intervals = kzalloc(sizeof(struct pcc_interval) *pcc_intervals*2,
				 GFP_KERNEL);
	if (!ca->intervals) {
		printk(KERN_INFO "init fails\n");
		return;
	}
    ++id;
    ca->id =id;
    ca->amplifier =pcc_min_amp;
    ca->swing_buffer=0;
    ca->change_bound=pcc_min_change_bound;
    ca->rate =pcc_min_rate*512;
    ca->last_rate =pcc_min_rate*512;
    ca->start_mode =true;
    ca->moving=false;
    ca->intervals[0].utility=S64_MIN;
    ca->util_func=&pcc_calc_utility_vivace_latency;

    pcc_setup_intervals_probing(ca);
    start_interval(sk,ca);
    cmpxchg(&sk->sk_pacing_status,SK_PACING_NONE,SK_PACING_NEEDED);
    // ic_pcc_reset(sk);

}

static void ic_release(struct sock *sk)
{
    struct ic *ca = inet_csk_ca(sk);
    kfree(ca->intervals);

}


static void ic_cong_control(struct sock *sk,const struct rate_sample *rs)
{
    struct tcp_sock *tp=tcp_sk(sk);
    struct ic *ca = inet_csk_ca(sk);
    pcc_process(ca,sk);
    //receiving ack means ascendent

}

// not happen in CWR and recovery etc 
// increase tp->snd_cwnd
static void ic_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
//     struct tcp_sock *tp = tcp_sk(sk);
// 	struct ic *ca = inet_csk_ca(sk);
    
}

/*MD phase*/
static u32 ic_ssthresh(struct sock *sk){
    const struct tcp_sock *tp = tcp_sk(sk);
    struct ic *ca = inet_csk_ca(sk);
    ca->temp_cwnd = tp->snd_cwnd;
    if(add_cwnd < 0){
        ca->start_mode =true;
        return max(tp->snd_cwnd >> 1U, 2U);
    }
    else
    {
        //????????????????????????????????????????????????????????????
        return tp->snd_cwnd;
    }
    

}



static void ic_state(struct sock *sk, u8 new_state)
{
	struct ic *ca = inet_csk_ca(sk);
	s32 spare;

	if (!pcc_valid(ca))
		return;
    
    // if(new_state == TCP_CA_Loss && add_cwnd >=0){
    //     tp->snd_cwnd =ca->temp_cwnd;
    // }
	if (ca->loss_state && new_state != 4) {
		spare = tcp_sk(sk)->delivered + tcp_sk(sk)->lost+
			tcp_packets_in_flight(tcp_sk(sk));
		spare -= tcp_sk(sk)->data_segs_out;
		spare -= ca->spare;
		ca->spare+= spare;
		printk(KERN_INFO "%d loss ended: spare %d\n", ca->id, spare);

		ca->loss_state = false;
		pcc_setup_intervals_probing(ca);
		start_interval(sk, ca);
	}
	else if (!ca->loss_state && new_state	== 4) {
		printk(KERN_INFO "%d loss: started\n", ca->id);
		ca->loss_state = true;
		ca->wait = true;
		start_interval(sk, ca);
	}
    
}

static u32 ic_undo_cwnd(struct sock *sk){
    const struct tcp_sock *tp = tcp_sk(sk);

    /*returns the congestion window of a flow, after a false loss detection (due to fasle timeout or packet reordering)
    is confirmed*/
    return max(tp->snd_cwnd, tp->prior_cwnd);

}

static struct tcp_congestion_ops  tcp_ic __read_mostly ={
        .flags = TCP_CONG_NON_RESTRICTED,
        .init =  ic_init,
        .cong_avoid =  ic_cong_avoid,
        .cong_control =  ic_cong_control,
        .set_state =  ic_state,
        .undo_cwnd=  ic_undo_cwnd,
        .ssthresh =  ic_ssthresh,
        .release =  ic_release,
        .owner =  THIS_MODULE,
        .name =  "ic"
};

static int __init ic_register(void)
{
    BUILD_BUG_ON(sizeof(struct ic) > ICSK_CA_PRIV_SIZE);

    /* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */
    //beta_scale is about 15 8*(1024+717)/3/(1024-717);
	// beta_scale = 8*(BICTCP_BETA_SCALE+beta) / 3 / (BICTCP_BETA_SCALE - beta);
        //value is 1024 * c / rtt ???about 410
	// cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */

    /* 1/c * 2^2*bictcp_HZ * srtt */
	// cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */

    // do_div(cube_factor, bic_scale * 10);

    printk(KERN_INFO "ic init reg\n");

    return tcp_register_congestion_control(&tcp_ic);

}

static void __exit ic_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_ic);
}

module_init(ic_register);
module_exit(ic_unregister);

MODULE_AUTHOR("XX");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ic tcp");
MODULE_VERSION("1.0");


