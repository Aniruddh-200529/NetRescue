#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netrescue.h"
#include "arq.h"
#include "topology.h"

static NrNetwork g_network;
static NrArqSimulation g_arq;
static NrArqConfig g_config;
static NrTopologyType g_topology = NR_TOPO_HYBRID;
static unsigned g_seed = 424242U;
static unsigned g_chart_metric = 0U;
static int g_source_id = -1, g_destination_id = -1;
static bool g_running = false, g_dragging = false, g_link_mode = false;
static bool g_has_comparison = false;
static NrArqComparisonResult g_compare_gbn, g_compare_sr;
static int g_selected_node = -1, g_selected_link = -1;
static int g_drag_node = -1;
static int g_link_source_id = -1;
static HWND g_window;
static HDC g_back_dc;
static HBITMAP g_back_bitmap;
static HGDIOBJ g_back_previous_bitmap;
static int g_back_width, g_back_height;
static HWND g_name_edit;
static WNDPROC g_original_edit_proc;
static const COLORREF BG = RGB(5, 14, 26), PANEL = RGB(10, 26, 42), CYAN = RGB(49, 179, 255), TEXT = RGB(218, 235, 250);
static const char *chart_metric_name(unsigned metric){static const char *names[]={"Delivery ratio","Throughput Mbps","Average latency ms","Queue utilization","Packet loss rate"};return metric<5U?names[metric]:names[0];}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c) { HBRUSH b = CreateSolidBrush(c); RECT r = {x,y,x+w,y+h}; FillRect(dc,&r,b); DeleteObject(b); }
static void label(HDC dc, int x, int y, const char *s, int size, COLORREF c, bool bold) {
    HFONT f = CreateFontA(size,0,0,0,bold?FW_SEMIBOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
    HGDIOBJ old=SelectObject(dc,f); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,c); TextOutA(dc,x,y,s,(int)strlen(s)); SelectObject(dc,old); DeleteObject(f);
}
static void button(HDC dc, int x, int y, int w, const char *s, COLORREF c) { fill(dc,x,y,w,34,c); label(dc,x+13,y+8,s,14,RGB(245,250,255),true); }
static POINT node_point(const NrNode *n, int width, int height) { POINT p={(LONG)(70.0+(double)n->x*(double)(width-140)),(LONG)(65.0+(double)n->y*(double)(height-245))}; return p; }
static COLORREF node_color(const NrNode *n) { if (!n->active) return RGB(245,65,78); switch(n->type){case NR_HOST:return RGB(30,155,255);case NR_ROUTER:return RGB(250,65,75);case NR_SWITCH:return RGB(40,215,130);case NR_SERVER:return RGB(165,105,255);case NR_AP:return RGB(20,195,220);}return CYAN; }
static bool route_has_edge(const NrRoute *route,int a,int b){if(route==NULL||!route->reachable)return false;for(size_t i=0;i+1U<route->length;i++)if((route->nodes[i]==a&&route->nodes[i+1U]==b)||(route->nodes[i]==b&&route->nodes[i+1U]==a))return true;return false;}
static void draw(HDC dc, RECT client) {
    int w=client.right,h=client.bottom; fill(dc,0,0,w,h,BG); fill(dc,0,0,w,64,RGB(7,18,32));
    label(dc,22,10,"NR",25,RGB(255,75,88),true); label(dc,67,7,"NetRescue",25,TEXT,true); label(dc,68,37,"FAULT-TOLERANT NETWORK SIMULATION  /  DYNAMIC RECOVERY",10,RGB(114,179,214),true);
    const char *run_label = g_arq.state == NR_ARQ_RUNNING ? "|| Pause" : g_arq.state == NR_ARQ_PAUSED ? "> Resume" : "> Start";
    button(dc,w-500,15,112,run_label,g_arq.state==NR_ARQ_RUNNING?RGB(18,138,108):RGB(17,99,215)); button(dc,w-380,15,118,g_arq.state==NR_ARQ_RUNNING||g_arq.state==NR_ARQ_PAUSED?"Stop Traffic":"Run Traffic",RGB(13,135,118)); button(dc,w-252,15,112,"Fail Link",RGB(176,47,57)); button(dc,w-132,15,104,"Reset",RGB(76,88,112));
    fill(dc,12,78,220,h-94,PANEL); fill(dc,244,78,w-490,h-94,RGB(8,22,38)); fill(dc,w-232,78,220,h-94,PANEL);
    label(dc,28,94,"NETWORK BUILDER",13,CYAN,true); label(dc,28,124,"NODES",10,RGB(105,157,194),true);
    for(size_t i=0;i<5;i++){char line[64];(void)snprintf(line,sizeof(line),"+  Add %s",nr_node_type_name((NrNodeType)i));label(dc,30,148+(int)i*31,line,13,TEXT,false);}
    label(dc,28,320,"TOPOLOGY TEMPLATES  (click)",10,RGB(105,157,194),true);
    for(int type=0;type<NR_TOPO_COUNT;type++){bool selected=g_topology==(NrTopologyType)type;char item[64];(void)snprintf(item,sizeof(item),"%c  %s",selected?'>' : ' ',nr_topology_name((NrTopologyType)type));label(dc,30,342+type*23,item,12,selected?CYAN:TEXT,selected);}
    label(dc,28,540,"DRAG NODES  |  F: FAIL LINK",10,RGB(105,157,194),true); label(dc,30,561,"F2 rename / T type / L link",11,TEXT,false);label(dc,30,581,"1/2: set endpoints  M: control",10,TEXT,false);
    label(dc,28,625,"RELIABILITY",10,RGB(105,157,194),true); char score[32];double reliability=g_arq.stats.generated?100.0*nr_arq_delivery_ratio(&g_arq):nr_reliability(&g_network);(void)snprintf(score,sizeof(score),"%.1f / 100",reliability); label(dc,28,647,score,23,RGB(58,232,156),true);
    int canvasw=w-490, canvash=h-94; for(int x=20;x<canvasw;x+=40) fill(dc,244+x,78,1,canvash,RGB(14,34,51)); for(int y=20;y<canvash;y+=40) fill(dc,244,78+y,canvasw,1,RGB(14,34,51));
    /* Draw links from the same graph the router uses. */
    for(size_t i=0;i<g_network.link_count;i++){
        NrLink *l=&g_network.links[i]; NrNode *a=nr_find_node(&g_network,l->from),*b=nr_find_node(&g_network,l->to);if(!a||!b)continue;
        POINT pa=node_point(a,canvasw,canvash),pb=node_point(b,canvasw,canvash);pa.x+=244;pa.y+=78;pb.x+=244;pb.y+=78;
        bool active_route=route_has_edge(&g_arq.forward_route,l->from,l->to);
        COLORREF col=(!l->active||!a->active||!b->active)?RGB(239,56,69):(active_route?CYAN:(l->utilization>.65?RGB(252,190,53):RGB(59,128,174)));
        HPEN pen=CreatePen(PS_SOLID,(g_selected_link==l->id?4:2),col);HGDIOBJ old=SelectObject(dc,pen);MoveToEx(dc,pa.x,pa.y,NULL);LineTo(dc,pb.x,pb.y);SelectObject(dc,old);DeleteObject(pen);
        if(!l->active){int mx=(pa.x+pb.x)/2,my=(pa.y+pb.y)/2;label(dc,mx-7,my-13,"X",24,RGB(255,70,74),true);}
    }
    /* Packets travel continuously along their current graph edge. */
    for(size_t i=0;i<g_network.packet_count;i++){NrPacket *p=&g_network.packets[i];if(p->state!=NR_PACKET_IN_FLIGHT||p->path_index+1>=p->path_length)continue;
        NrNode *a=nr_find_node(&g_network,p->path[p->path_index]),*b=nr_find_node(&g_network,p->path[p->path_index+1]);if(!a||!b)continue;POINT pa=node_point(a,canvasw,canvash),pb=node_point(b,canvasw,canvash);
        int px=244+pa.x+(int)((pb.x-pa.x)*p->hop_progress),py=78+pa.y+(int)((pb.y-pa.y)*p->hop_progress);fill(dc,px-4,py-4,9,9,RGB(73,216,255));}
    for(size_t i=0;i<g_arq.event_count;i++){NrArqEvent *event=&g_arq.events[i];if(event->due_at<=g_arq.time_seconds)continue;NrRoute *route=event->type==NR_ARQ_ACK_EVENT?&g_arq.reverse_route:&g_arq.forward_route;if(route->length<2U)continue;double duration=event->due_at-event->started_at;double progress=duration<=0.0?1.0:(g_arq.time_seconds-event->started_at)/duration;if(progress<0.0)progress=0.0;if(progress>1.0)progress=1.0;double hops=progress*(double)(route->length-1U);size_t hop=(size_t)hops;if(hop>=route->length-1U)hop=route->length-2U;double local=hops-(double)hop;NrNode *a=nr_find_node(&g_network,route->nodes[hop]),*b=nr_find_node(&g_network,route->nodes[hop+1U]);if(!a||!b)continue;POINT pa=node_point(a,canvasw,canvash),pb=node_point(b,canvasw,canvash);int px=244+pa.x+(int)((double)(pb.x-pa.x)*local),py=78+pa.y+(int)((double)(pb.y-pa.y)*local);COLORREF packet_color=event->lost||event->corrupted?RGB(255,67,74):event->type==NR_ARQ_ACK_EVENT?RGB(69,225,133):event->attempt>1U?RGB(179,101,255):RGB(73,216,255);fill(dc,px-4,py-4,9,9,packet_color);}
    for(size_t i=0;i<g_network.node_count;i++){NrNode *n=&g_network.nodes[i];POINT p=node_point(n,canvasw,canvash);p.x+=244;p.y+=78;COLORREF col=node_color(n);
        HBRUSH brush=CreateSolidBrush(col);HPEN pen=CreatePen(PS_SOLID,2,g_selected_node==n->id?RGB(255,255,255):col);HGDIOBJ ob=SelectObject(dc,brush),op=SelectObject(dc,pen);Ellipse(dc,p.x-19,p.y-19,p.x+19,p.y+19);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(brush);DeleteObject(pen);
        const char *glyph=n->type==NR_HOST?"PC":n->type==NR_ROUTER?"R":n->type==NR_SWITCH?"S":n->type==NR_SERVER?"DB":"AP";label(dc,p.x-9,p.y-8,glyph,13,RGB(255,255,255),true);label(dc,p.x-12,p.y+23,n->name,12,TEXT,true);
    }
    label(dc,263,91,"LIVE TOPOLOGY",12,TEXT,true); label(dc,w-218,94,"INSPECTOR",12,CYAN,true);
    char buf[160]; NrNode *sel=nr_find_node(&g_network,g_selected_node);if(sel){(void)snprintf(buf,sizeof(buf),"%s  /  %s",sel->name,nr_node_type_name(sel->type));label(dc,w-218,126,buf,14,TEXT,true);(void)snprintf(buf,sizeof(buf),"IP  %s",sel->ip);label(dc,w-218,154,buf,12,TEXT,false);(void)snprintf(buf,sizeof(buf),"Queue  %u / %u",sel->queue_size,sel->queue_capacity);label(dc,w-218,180,buf,12,TEXT,false);label(dc,w-218,210,sel->active?"o  Active":"o  Failed",12,sel->active?RGB(55,225,145):RGB(255,75,80),true);}else label(dc,w-218,126,"Select a node to inspect",12,TEXT,false);
    NrLink *selected_link=nr_find_link(&g_network,g_selected_link);if(selected_link!=NULL){(void)snprintf(buf,sizeof(buf),"Link %d  %s",selected_link->id,selected_link->active?"Active":"Failed");label(dc,w-218,126,buf,14,selected_link->active?TEXT:RGB(255,75,80),true);(void)snprintf(buf,sizeof(buf),"BW %.1f Mbps",selected_link->bandwidth_mbps);label(dc,w-218,154,buf,12,TEXT,false);(void)snprintf(buf,sizeof(buf),"Delay %.1f ms  Cost %.1f",selected_link->delay_ms,selected_link->cost);label(dc,w-218,180,buf,11,TEXT,false);(void)snprintf(buf,sizeof(buf),"Loss %.0f%%  %s",selected_link->loss_probability*100.0,selected_link->bidirectional?"Bidirectional":"Directed");label(dc,w-218,210,buf,11,TEXT,false);label(dc,w-218,232,"Click rows to edit link properties",9,RGB(105,157,194),false);}else{(void)snprintf(buf,sizeof(buf),"Payload %u B  [ / ] adjusts",g_config.payload_bytes);label(dc,w-218,232,buf,10,RGB(105,157,194),false);}
    label(dc,w-218,264,"SIMULATION",11,RGB(105,157,194),true);(void)snprintf(buf,sizeof(buf),"Nodes     %zu",g_network.node_count);label(dc,w-218,290,buf,12,TEXT,false);(void)snprintf(buf,sizeof(buf),"Route %d -> %d",g_source_id,g_destination_id);label(dc,w-218,315,buf,12,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Protocol  %s",nr_arq_protocol_name(g_config.protocol));label(dc,w-218,352,buf,12,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Status    %s",nr_arq_state_name(g_arq.state));label(dc,w-218,377,buf,12,g_arq.state==NR_ARQ_COMPLETE?RGB(61,225,150):TEXT,true);
    (void)snprintf(buf,sizeof(buf),"Packets   %llu / %zu",(unsigned long long)g_arq.stats.delivered,g_config.packet_count);label(dc,w-218,402,buf,12,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Retry %llu  Loss %llu",(unsigned long long)g_arq.stats.retransmissions,(unsigned long long)g_arq.stats.packet_losses);label(dc,w-218,427,buf,12,RGB(250,190,72),false);
    (void)snprintf(buf,sizeof(buf),"Window [%u]  click - / +",g_config.protocol==NR_ARQ_STOP_WAIT?1U:g_config.window_size);label(dc,w-218,452,buf,11,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Data loss %.0f%%  click - / +",g_config.loss_probability*100.0);label(dc,w-218,477,buf,11,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"ACK loss %.0f%%  click - / +",g_config.ack_loss_probability*100.0);label(dc,w-218,502,buf,10,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Packets %zu   Rate %.0f/s",g_config.packet_count,g_config.packets_per_second);label(dc,w-218,527,buf,11,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Data corrupt %.0f%%  click - / +",g_config.corruption_probability*100.0);label(dc,w-218,552,buf,10,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"ACK corrupt %.0f%%  click - / +",g_config.ack_corruption_probability*100.0);label(dc,w-218,577,buf,10,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Timeout %.0fms  Seed %u",g_config.timeout_seconds*1000.0,g_seed);label(dc,w-218,602,buf,10,RGB(105,157,194),false);
    (void)snprintf(buf,sizeof(buf),"Tx %llu  ACK %llu  TO %llu",(unsigned long long)g_arq.stats.data_transmissions,(unsigned long long)g_arq.stats.acknowledgements,(unsigned long long)g_arq.stats.timeouts);label(dc,w-218,628,buf,10,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Avg %.1f ms  Max %.1f ms",g_arq.stats.delivered?g_arq.stats.total_latency_ms/(double)g_arq.stats.delivered:0.0,g_arq.stats.max_latency_ms);label(dc,w-218,651,buf,10,TEXT,false);
    (void)snprintf(buf,sizeof(buf),"Queue %.0f%%  CC %s  drops %llu",g_arq.queue_utilization*100.0,g_config.congestion_control?"ON":"OFF",(unsigned long long)g_arq.stats.queue_drops);label(dc,w-218,676,buf,10,TEXT,false);
    fill(dc,12,h-148,w-24,136,RGB(8,19,32)); label(dc,27,h-137,"SIMULATION EVENTS",11,CYAN,true);
    size_t show=g_arq.log_count<4U?g_arq.log_count:4U;for(size_t j=0;j<show;j++){size_t ix=(g_arq.log_cursor+128U-g_arq.log_count+j)%128U;label(dc,27,h-114+(int)j*18,g_arq.log[ix],10,TEXT,false);}
    int chart_x=w/2, chart_y=h-125, chart_w=w/2-34, chart_h=95;char chart_title[128];(void)snprintf(chart_title,sizeof(chart_title),"%s (click to cycle)  |  V: GBN vs SR",chart_metric_name(g_chart_metric));label(dc,chart_x,h-137,chart_title,10,CYAN,true);
    for(int gy=0;gy<4;gy++){int yy=chart_y+gy*chart_h/3;HPEN p=CreatePen(PS_SOLID,1,RGB(24,48,65));HGDIOBJ old=SelectObject(dc,p);MoveToEx(dc,chart_x,yy,NULL);LineTo(dc,chart_x+chart_w,yy);SelectObject(dc,old);DeleteObject(p);}
    if(g_arq.history_count>1U){double maximum=1.0;if(g_chart_metric==1U||g_chart_metric==2U){maximum=0.0;for(size_t i=0;i<g_arq.history_count;i++){double value=g_chart_metric==1U?g_arq.history[i].throughput_mbps:g_arq.history[i].average_latency_ms;if(value>maximum)maximum=value;}if(maximum<=0.0)maximum=1.0;}
      COLORREF metric_color=g_chart_metric==0U?RGB(53,226,138):g_chart_metric==1U?CYAN:g_chart_metric==2U?RGB(184,111,255):g_chart_metric==3U?RGB(250,190,72):RGB(255,90,90);HPEN p=CreatePen(PS_SOLID,2,metric_color);HGDIOBJ old=SelectObject(dc,p);
      for(size_t i=0;i<g_arq.history_count;i++){NrArqSample *sample=&g_arq.history[i];double value=g_chart_metric==0U?sample->delivery_ratio:g_chart_metric==1U?sample->throughput_mbps:g_chart_metric==2U?sample->average_latency_ms:g_chart_metric==3U?sample->queue_utilization:sample->packet_loss_rate;int px=chart_x+(int)(i*(size_t)chart_w/(g_arq.history_count-1U));int py=chart_y+chart_h-(int)(value/maximum*(double)chart_h);if(i==0U)MoveToEx(dc,px,py,NULL);else LineTo(dc,px,py);}
      SelectObject(dc,old);DeleteObject(p);}
    if(g_has_comparison){(void)snprintf(buf,sizeof(buf),"GBN %llu tx / %.2fs / %.0f%%   SR %llu tx / %.2fs / %.0f%%",(unsigned long long)g_compare_gbn.statistics.data_transmissions,g_compare_gbn.duration_seconds,g_compare_gbn.efficiency*100.0,(unsigned long long)g_compare_sr.statistics.data_transmissions,g_compare_sr.duration_seconds,g_compare_sr.efficiency*100.0);label(dc,chart_x,h-26,buf,9,TEXT,false);}
    label(dc,w-234,h-88,g_arq.state==NR_ARQ_RUNNING?"o  RUNNING":g_arq.state==NR_ARQ_COMPLETE?"o  COMPLETE":g_arq.state==NR_ARQ_FAILED?"o  FAILED":"o  PAUSED / IDLE",11,g_arq.state==NR_ARQ_RUNNING?RGB(56,225,150):g_arq.state==NR_ARQ_FAILED?RGB(255,75,80):RGB(245,187,72),true);
}
static void release_back_buffer(void) {
    if (g_back_dc != NULL) {
        if (g_back_previous_bitmap != NULL) (void)SelectObject(g_back_dc, g_back_previous_bitmap);
        (void)DeleteDC(g_back_dc);
    }
    if (g_back_bitmap != NULL) (void)DeleteObject(g_back_bitmap);
    g_back_dc = NULL;
    g_back_bitmap = NULL;
    g_back_previous_bitmap = NULL;
    g_back_width = 0;
    g_back_height = 0;
}
static bool ensure_back_buffer(HWND hwnd, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (g_back_dc != NULL && g_back_bitmap != NULL &&
        g_back_width == width && g_back_height == height) return true;

    HDC window_dc = GetDC(hwnd);
    if (window_dc == NULL) return false;
    HDC new_dc = CreateCompatibleDC(window_dc);
    HBITMAP new_bitmap = new_dc != NULL ? CreateCompatibleBitmap(window_dc, width, height) : NULL;
    (void)ReleaseDC(hwnd, window_dc);
    if (new_dc == NULL || new_bitmap == NULL) {
        if (new_bitmap != NULL) (void)DeleteObject(new_bitmap);
        if (new_dc != NULL) (void)DeleteDC(new_dc);
        return false;
    }

    HGDIOBJ previous = SelectObject(new_dc, new_bitmap);
    if (previous == NULL || previous == HGDI_ERROR) {
        (void)DeleteObject(new_bitmap);
        (void)DeleteDC(new_dc);
        return false;
    }

    release_back_buffer();
    g_back_dc = new_dc;
    g_back_bitmap = new_bitmap;
    g_back_previous_bitmap = previous;
    g_back_width = width;
    g_back_height = height;
    return true;
}
static void defaults(void){memset(&g_config,0,sizeof(g_config));g_config.protocol=NR_ARQ_GO_BACK_N;g_config.packet_count=20U;g_config.window_size=4U;g_config.payload_bytes=128U;g_config.packets_per_second=20.0;g_config.timeout_seconds=.18;g_config.seed=g_seed;g_config.congestion_control=true;}
static LRESULT CALLBACK name_edit_proc(HWND edit,UINT msg,WPARAM wp,LPARAM lp){if(msg==WM_KEYDOWN&&(wp==VK_RETURN||wp==VK_ESCAPE)){PostMessageA(GetParent(edit),WM_APP+1,wp==VK_RETURN?1:0,0);return 0;}if(msg==WM_KILLFOCUS){PostMessageA(GetParent(edit),WM_APP+1,1,0);}return CallWindowProcA(g_original_edit_proc,edit,msg,wp,lp);}
static void begin_node_rename(HWND hwnd){NrNode *node=nr_find_node(&g_network,g_selected_node);if(node==NULL||g_arq.state==NR_ARQ_RUNNING||g_arq.state==NR_ARQ_PAUSED)return;if(g_name_edit!=NULL)DestroyWindow(g_name_edit);RECT r;GetClientRect(hwnd,&r);g_name_edit=CreateWindowExA(0,"EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,r.right-220,122,190,25,hwnd,NULL,GetModuleHandleA(NULL),NULL);if(g_name_edit){SetWindowTextA(g_name_edit,node->name);g_original_edit_proc=(WNDPROC)SetWindowLongPtrA(g_name_edit,GWLP_WNDPROC,(LONG_PTR)name_edit_proc);SetFocus(g_name_edit);SendMessageA(g_name_edit,EM_SETSEL,0,-1);}}
static void finish_node_rename(HWND hwnd,bool commit){if(g_name_edit==NULL)return;if(commit){char name[NR_NAME_LEN];NrNode *node=nr_find_node(&g_network,g_selected_node);int length=GetWindowTextA(g_name_edit,name,(int)sizeof(name));if(node!=NULL&&length>0)(void)snprintf(node->name,sizeof(node->name),"%s",name);}DestroyWindow(g_name_edit);g_name_edit=NULL;g_original_edit_proc=NULL;SetFocus(hwnd);}
static void reset_demo(void){(void)nr_load_demo(&g_network);memset(&g_arq,0,sizeof(g_arq));defaults();g_topology=(NrTopologyType)-1;g_has_comparison=false;g_selected_node=-1;g_selected_link=-1;g_source_id=g_network.nodes[0].id;g_destination_id=g_network.nodes[g_network.node_count-1U].id;}
static void reset_simulation(void){memset(&g_arq,0,sizeof(g_arq));g_running=false;g_has_comparison=false;g_network.packet_count=0U;g_network.time_seconds=0.0;memset(&g_network.stats,0,sizeof(g_network.stats));for(size_t i=0;i<g_network.link_count;i++)g_network.links[i].active=true;for(size_t i=0;i<g_network.node_count;i++)g_network.nodes[i].active=true;g_source_id=g_network.node_count?g_network.nodes[0].id:-1;g_destination_id=g_network.node_count?g_network.nodes[g_network.node_count-1U].id:-1;}
static void send_packet(void){if(g_source_id<0||g_destination_id<0||nr_find_node(&g_network,g_source_id)==NULL||nr_find_node(&g_network,g_destination_id)==NULL)return;g_config.seed=g_seed;if(nr_arq_start(&g_arq,&g_network,&g_config,g_source_id,g_destination_id))g_running=true;else MessageBeep(MB_ICONWARNING);}
static void start_pause(void){if(g_arq.state==NR_ARQ_RUNNING){nr_arq_pause(&g_arq);g_running=false;}else if(g_arq.state==NR_ARQ_PAUSED){nr_arq_resume(&g_arq);g_running=true;}else send_packet();}
static void toggle_traffic(void){if(g_arq.state==NR_ARQ_RUNNING||g_arq.state==NR_ARQ_PAUSED){nr_arq_stop(&g_arq);g_running=false;}else send_packet();}
static void compare_protocols(void){if(g_running||!nr_arq_compare(&g_network,&g_config,g_source_id,g_destination_id,&g_compare_gbn,&g_compare_sr)){MessageBeep(MB_ICONWARNING);return;}g_has_comparison=true;char message[NR_EVENT_LEN];(void)snprintf(message,sizeof(message),"Compare GBN %llu tx %.2fs; SR %llu tx %.2fs",(unsigned long long)g_compare_gbn.statistics.data_transmissions,g_compare_gbn.duration_seconds,(unsigned long long)g_compare_sr.statistics.data_transmissions,g_compare_sr.duration_seconds);nr_arq_log(&g_arq,message);}
static void topology_file(HWND hwnd,bool save){char path[MAX_PATH]="";OPENFILENAMEA dialog;memset(&dialog,0,sizeof(dialog));dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=hwnd;dialog.lpstrFilter="NetRescue topology (*.topo)\0*.topo\0All files (*.*)\0*.*\0\0";dialog.lpstrFile=path;dialog.nMaxFile=MAX_PATH;dialog.lpstrDefExt="topo";dialog.Flags=OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);if(!(save?GetSaveFileNameA(&dialog):GetOpenFileNameA(&dialog)))return;if(save){if(!nr_save_topology(&g_network,path))MessageBoxA(hwnd,"Could not save topology.","NetRescue",MB_ICONERROR);}else if(g_arq.state==NR_ARQ_RUNNING||g_arq.state==NR_ARQ_PAUSED){MessageBeep(MB_ICONWARNING);}else if(!nr_load_topology(&g_network,path)){MessageBoxA(hwnd,"The selected file is not a valid topology.","NetRescue",MB_ICONERROR);}else{memset(&g_arq,0,sizeof(g_arq));g_running=false;g_has_comparison=false;g_topology=(NrTopologyType)-1;g_selected_node=-1;g_selected_link=-1;g_source_id=g_network.node_count?g_network.nodes[0].id:-1;g_destination_id=g_network.node_count?g_network.nodes[g_network.node_count-1U].id:-1;}}
static void select_topology(NrTopologyType type){if(g_arq.state==NR_ARQ_RUNNING||g_arq.state==NR_ARQ_PAUSED){MessageBeep(MB_ICONWARNING);return;}if(!nr_generate_topology(&g_network,type,8U,g_seed))return;g_topology=type;g_source_id=g_network.nodes[0].id;g_destination_id=g_network.nodes[g_network.node_count-1U].id;g_selected_link=-1;g_selected_node=-1;memset(&g_arq,0,sizeof(g_arq));g_running=false;}
static void toggle_selected_link(void){if(g_selected_link<0&&g_network.link_count>0U)g_selected_link=g_network.links[0].id;NrLink *l=nr_find_link(&g_network,g_selected_link);if(l)(void)nr_set_link_active(&g_network,l->id,!l->active);}
static LRESULT CALLBACK wndproc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){case WM_CREATE:SetTimer(hwnd,1,30,NULL);return 0;
    case WM_SIZE:{RECT r;if(GetClientRect(hwnd,&r)&&r.right>0&&r.bottom>0)(void)ensure_back_buffer(hwnd,r.right,r.bottom);InvalidateRect(hwnd,NULL,FALSE);return 0;}
    case WM_DISPLAYCHANGE:{release_back_buffer();RECT r;if(GetClientRect(hwnd,&r)&&r.right>0&&r.bottom>0)(void)ensure_back_buffer(hwnd,r.right,r.bottom);InvalidateRect(hwnd,NULL,FALSE);return 0;}
    case WM_GETMINMAXINFO:{MINMAXINFO *limits=(MINMAXINFO *)lp;limits->ptMinTrackSize.x=1240;limits->ptMinTrackSize.y=760;return 0;}
    case WM_TIMER:if(wp==1&&g_running){nr_step(&g_network,.03);nr_arq_step(&g_arq,.03);if(g_arq.state!=NR_ARQ_RUNNING)g_running=false;InvalidateRect(hwnd,NULL,FALSE);}return 0;
    case WM_APP+1:finish_node_rename(hwnd,wp!=0);InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT r;if(GetClientRect(hwnd,&r)&&r.right>0&&r.bottom>0){if(ensure_back_buffer(hwnd,r.right,r.bottom)){draw(g_back_dc,r);if(!BitBlt(dc,0,0,r.right,r.bottom,g_back_dc,0,0,SRCCOPY))draw(dc,r);}else draw(dc,r);}EndPaint(hwnd,&ps);return 0;}
    case WM_KEYDOWN:if((wp=='S'||wp=='O')&&(GetKeyState(VK_CONTROL)&0x8000))topology_file(hwnd,wp=='S');else if(wp==VK_F2)begin_node_rename(hwnd);else if(wp==VK_SPACE)start_pause();else if(wp=='S')send_packet();else if(wp=='V')compare_protocols();else if(wp=='F')toggle_selected_link();else if(wp=='T'&&g_selected_node>0&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED){NrNode *node=nr_find_node(&g_network,g_selected_node);if(node){node->type=(NrNodeType)((node->type+1)%5);g_topology=(NrTopologyType)-1;}}else if(wp=='N'&&g_selected_node>0){NrNode *node=nr_find_node(&g_network,g_selected_node);if(node)(void)nr_set_node_active(&g_network,node->id,!node->active);}else if(wp=='K'&&g_selected_link>0&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED){NrLink *link=nr_find_link(&g_network,g_selected_link);if(link){link->bidirectional=!link->bidirectional;g_topology=(NrTopologyType)-1;}}else if(wp=='M')g_config.congestion_control=!g_config.congestion_control;else if(wp=='R')reset_simulation();else if(wp=='1'&&g_selected_node>0)g_source_id=g_selected_node;else if(wp=='2'&&g_selected_node>0)g_destination_id=g_selected_node;else if(wp=='P')g_config.protocol=(NrArqProtocol)((g_config.protocol+1)%NR_ARQ_PROTOCOL_COUNT);else if(wp=='C')g_config.corruption_probability=g_config.corruption_probability>=.99?0.0:fmin(1.0,g_config.corruption_probability+.1);else if(wp=='X')g_config.loss_probability=g_config.loss_probability>=.99?0.0:fmin(1.0,g_config.loss_probability+.1);else if(wp==VK_OEM_4&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED)g_config.payload_bytes=g_config.payload_bytes>64U?g_config.payload_bytes-64U:1U;else if(wp==VK_OEM_6&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED)g_config.payload_bytes=g_config.payload_bytes<65471U?g_config.payload_bytes+64U:65535U;else if(wp==VK_ADD||wp==VK_OEM_PLUS)g_config.packets_per_second=fmin(10000.0,g_config.packets_per_second+5.0);else if(wp==VK_SUBTRACT||wp==VK_OEM_MINUS)g_config.packets_per_second=fmax(0.0,g_config.packets_per_second-5.0);else if(wp=='G'){g_seed=(unsigned)GetTickCount();g_config.seed=g_seed;}else if(wp=='L'&&g_selected_node>0&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED){if(g_link_source_id<0){g_link_source_id=g_selected_node;g_link_mode=true;}else if(g_link_source_id!=g_selected_node&&nr_add_link(&g_network,g_link_source_id,g_selected_node,true,1.0,10.0,100.0)>0){g_link_source_id=-1;g_link_mode=false;g_topology=(NrTopologyType)-1;}}else if(wp==VK_DELETE&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED){if(g_selected_link>0){(void)nr_remove_link(&g_network,g_selected_link);g_selected_link=-1;g_topology=(NrTopologyType)-1;}else if(g_selected_node>0){(void)nr_remove_node(&g_network,g_selected_node);g_selected_node=-1;g_topology=(NrTopologyType)-1;}}else if(wp==VK_F5)reset_demo();else if(wp==VK_ESCAPE){g_selected_node=-1;g_selected_link=-1;g_link_source_id=-1;g_link_mode=false;}InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_LBUTTONDOWN:{int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);RECT r;GetClientRect(hwnd,&r);int w=r.right,canvasw=w-490,canvash=r.bottom-94;
      if(y<60&&x>w-500&&x<w-388)start_pause();else if(y<60&&x>w-380&&x<w-262)toggle_traffic();else if(y<60&&x>w-252&&x<w-140)toggle_selected_link();else if(y<60&&x>w-132)reset_simulation();
      else if(x<232&&y>=342&&y<342+NR_TOPO_COUNT*23)select_topology((NrTopologyType)((y-342)/23));
      else if(x>w-232&&g_selected_link>0&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED&&y>=140&&y<245){NrLink *link=nr_find_link(&g_network,g_selected_link);if(link&&y<170)link->bandwidth_mbps=fmax(.1,fmin(1000000.0,link->bandwidth_mbps+(x<w-122?-.5:.5)));else if(link&&y<200)link->delay_ms=fmax(0.0,fmin(60000.0,link->delay_ms+(x<w-122?-1.0:1.0)));else if(link&&y<220)link->loss_probability=fmax(0.0,fmin(1.0,link->loss_probability+(x<w-122?-.05:.05)));else if(link)link->cost=fmax(0.0,link->cost+(x<w-122?-.5:.5));}
      else if(x>w-232&&g_selected_link<=0&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED&&y>=220&&y<245)g_config.payload_bytes=x<w-122?(g_config.payload_bytes>64U?g_config.payload_bytes-64U:1U):(g_config.payload_bytes<65471U?g_config.payload_bytes+64U:65535U);
      else if(x>w-232&&y>=340&&y<366)g_config.protocol=(NrArqProtocol)((g_config.protocol+1)%NR_ARQ_PROTOCOL_COUNT);
      else if(x>w-232&&y>=440&&y<465){if(x<w-122&&g_config.window_size>1)g_config.window_size/=2;else if(g_config.window_size<64)g_config.window_size*=2;}
      else if(x>w-232&&y>=465&&y<490)g_config.loss_probability=fmax(0.0,fmin(1.0,g_config.loss_probability+(x<w-122?-.05:.05)));
      else if(x>w-232&&y>=490&&y<515)g_config.ack_loss_probability=fmax(0.0,fmin(1.0,g_config.ack_loss_probability+(x<w-122?-.05:.05)));
      else if(x>w-232&&y>=515&&y<540){if(x<w-122){if(g_config.packet_count>1)g_config.packet_count--;}else if(g_config.packet_count<NR_ARQ_MAX_PACKETS)g_config.packet_count++;}
      else if(x>w-232&&y>=540&&y<565)g_config.corruption_probability=fmax(0.0,fmin(1.0,g_config.corruption_probability+(x<w-122?-.05:.05)));
      else if(x>w-232&&y>=565&&y<590)g_config.ack_corruption_probability=fmax(0.0,fmin(1.0,g_config.ack_corruption_probability+(x<w-122?-.05:.05)));
      else if(x>w-232&&y>=590&&y<615)g_config.timeout_seconds=fmax(.02,fmin(5.0,g_config.timeout_seconds+(x<w-122?-.02:.02)));
      else if(x>w/2&&y>r.bottom-148)g_chart_metric=(g_chart_metric+1U)%5U;
      else if(x<232&&y>=140&&y<295){if(g_arq.state==NR_ARQ_RUNNING||g_arq.state==NR_ARQ_PAUSED){MessageBeep(MB_ICONWARNING);}else{size_t type=(size_t)((y-140)/31);if(type<=4U){char name[NR_NAME_LEN];(void)snprintf(name,sizeof(name),"%s%zu",type==NR_HOST?"H":type==NR_ROUTER?"R":type==NR_SWITCH?"S":type==NR_SERVER?"SV":"AP",g_network.node_count+1U);float px=.2F+(float)(g_network.node_count%4U)*.18F;float py=.25F+(float)((g_network.node_count/4U)%3U)*.25F;int id=nr_add_node(&g_network,name,(NrNodeType)type,px,py);if(id>0){if(g_selected_node>0)(void)nr_add_link(&g_network,g_selected_node,id,true,1,10,100);g_selected_node=id;g_topology=(NrTopologyType)-1;}}}}
      else {for(size_t i=0;i<g_network.node_count;i++){POINT p=node_point(&g_network.nodes[i],canvasw,canvash);p.x+=244;p.y+=78;if(abs(p.x-x)<23&&abs(p.y-y)<23){g_selected_node=g_network.nodes[i].id;if(g_link_mode&&g_link_source_id>0&&g_link_source_id!=g_selected_node&&g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED){if(nr_add_link(&g_network,g_link_source_id,g_selected_node,true,1.0,10.0,100.0)>0){g_link_source_id=-1;g_link_mode=false;g_topology=(NrTopologyType)-1;}}else if(g_arq.state!=NR_ARQ_RUNNING&&g_arq.state!=NR_ARQ_PAUSED){g_drag_node=g_selected_node;g_dragging=true;SetCapture(hwnd);}break;}}
       if(g_drag_node<0)for(size_t i=0;i<g_network.link_count;i++){NrLink *l=&g_network.links[i];NrNode *a=nr_find_node(&g_network,l->from),*b=nr_find_node(&g_network,l->to);if(a&&b){POINT pa=node_point(a,canvasw,canvash),pb=node_point(b,canvasw,canvash);pa.x+=244;pa.y+=78;pb.x+=244;pb.y+=78;double dx=(double)(pb.x-pa.x),dy=(double)(pb.y-pa.y),den=dx*dx+dy*dy;double t=den==0?0:((x-pa.x)*dx+(y-pa.y)*dy)/den;if(t<0)t=0;if(t>1)t=1;double d=hypot((double)x-(pa.x+t*dx),(double)y-(pa.y+t*dy));if(d<7){g_selected_link=l->id;g_selected_node=-1;break;}}}}
      InvalidateRect(hwnd,NULL,FALSE);return 0;}
    case WM_MOUSEMOVE:if(g_dragging){RECT r;GetClientRect(hwnd,&r);NrNode *n=nr_find_node(&g_network,g_drag_node);if(n){int cw=r.right-490,ch=r.bottom-94;n->x=(float)((GET_X_LPARAM(lp)-244-70)/(double)(cw-140));n->y=(float)((GET_Y_LPARAM(lp)-78-65)/(double)(ch-245));if(n->x<0)n->x=0;if(n->x>1)n->x=1;if(n->y<0)n->y=0;if(n->y>1)n->y=1;g_topology=(NrTopologyType)-1;}InvalidateRect(hwnd,NULL,FALSE);}return 0;
    case WM_LBUTTONUP:g_dragging=false;g_drag_node=-1;ReleaseCapture();return 0;
    case WM_DESTROY:KillTimer(hwnd,1);release_back_buffer();PostQuitMessage(0);return 0;}
    return DefWindowProc(hwnd,msg,wp,lp);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR cmd,int show){(void)previous;(void)cmd;reset_demo();WNDCLASSA wc;memset(&wc,0,sizeof(wc));wc.lpfnWndProc=wndproc;wc.hInstance=instance;wc.lpszClassName="NetRescueDashboard";wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hbrBackground=NULL;RegisterClassA(&wc);g_window=CreateWindowExA(0,wc.lpszClassName,"NetRescue | Fault-Tolerant Network Simulation",WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1500,950,NULL,NULL,instance,NULL);if(!g_window)return 1;ShowWindow(g_window,show);MSG message;while(GetMessage(&message,NULL,0,0)>0){TranslateMessage(&message);DispatchMessage(&message);}return 0;}
