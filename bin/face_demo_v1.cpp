#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <memory.h>

#include <unistd.h>
#include <signal.h>

#include <string>
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

#include "mtcnn.hpp"
#include "face_align.hpp"
#include "feature_extractor.hpp"
#include "face_verify.hpp"
#include "face_mem_store.hpp"
#include "network_shell.hpp"
#include "json/json.h"
#include <glog/logging.h>

#include "utils.hpp"

#ifndef MODEL_DIR
#define MODEL_DIR "./models"
#endif

volatile int quit_flag=0;

#define CMD_STATUS_IDEL 0
#define CMD_STATUS_PENDING  1
#define CMD_STATUS_RUN      2
#define CMD_STATUS_DONE     3

#define arm64_sync  asm volatile("dsb sy" : : : "memory")
#define x86_sync    asm volatile("mfence":::"memory")

#ifdef __x86_64__
#define mem_sync x86_sync
#else
#define mem_sync arm64_sync
#endif

struct shell_cmd_para
{
	volatile int cmd_status; /* idle, pending,run,done */

	std::string op;

	unsigned int face_id;  
	std::string name;

	shell_cmd_para(void) { cmd_status=CMD_STATUS_IDEL;}
};

typedef void (*shell_exec_func_t)(shell_cmd_para *);

std::map<std::string,shell_exec_func_t> shell_exec_table;

feature_extractor * p_extractor;
face_verifier   * p_verifier;
face_mem_store * p_mem_store;
cv::Mat * p_cur_frame;
int current_frame_count=0;

int win_keep_limit;
int trace_pixels;
int UNKNOWN_FACE_ID_MAX;

float mtcnn_min_size;
float mtcnn_pnet_threshold;
float mtcnn_rnet_threshold;
float mtcnn_onet_threshold;
float mtcnn_factor;

int maxFaceNum;
int ver_delay_frame;

std::string known_faces_path;


struct face_window
{
	face_box box;
	unsigned int face_id;
	unsigned int frame_seq;
	float center_x;
	float center_y;
	std::string name;
	char title[128];
	std::list<float> score_stored;
	int add_score(float score)
	{
		if(score_stored.size() == 3)
		{
			score_stored.pop_front();
		}
		score_stored.push_back(score);
		return score_stored.size();
	}
	float get_avg_score()
	{
		float sum = 0;
		for(auto i:score_stored)
			sum += i;
		if (score_stored.size())
			return sum/score_stored.size();
		else
			return 0;
	}
};

std::vector<face_window*> face_win_list;

int get_new_unknown_face_id(void)
{
	static unsigned int current_id=0;

        return  (current_id++%UNKNOWN_FACE_ID_MAX);
}


shell_cmd_para * get_shell_cmd_para(void)
{
	static shell_cmd_para * p_para=new shell_cmd_para();

	return p_para;
}


void register_shell_executor(const std::string &name, shell_exec_func_t func)
{
	shell_exec_table[name]=func;
}

void execute_shell_command(shell_cmd_para * p_para)
{
	shell_exec_func_t func;

	std::map<std::string,shell_exec_func_t>::iterator it;

	it=shell_exec_table.find(p_para->op);

	if(it ==shell_exec_table.end())
	{
		std::cerr<<"Not such command: "<<p_para->op<<std::endl;
		p_para->cmd_status=CMD_STATUS_DONE;
		return;
	}

	p_para->cmd_status=CMD_STATUS_RUN;

	func=it->second;

	func(p_para);

	p_para->cmd_status=CMD_STATUS_DONE;
}

unsigned int get_new_registry_id(void)
{
	static unsigned int register_id=10000;

	register_id++;

	if(register_id==20000)
		register_id=10000;

	return register_id;
}

static void exec_register_face_feature_from_pics(char *p_path, mtcnn* p_mtcnn)
{
	std::cout<<"Registered faces:";
	unsigned int face_id = 0;
	unsigned int i = 0;
	std::vector<string> face_names;

	for (auto & p : fs::directory_iterator(p_path)){
		string dir = 
		string face_name = fs::basename(p);
		string ext = fs::extension(p);
        std::cout << face_name + ext << std::endl;

		if (ext != ".jpg" || ext != ".png" || ext != ".jpeg")
			continue;

		face_info info;
		info.p_feature = (float*)malloc(256*sizeof(float));

		cv::Mat img = cv::imread(p);
		/* detect face*/
		std::vector<face_box> faces_box;
		p_mtcnn->detect(img, faces_box);
		/* align face */
		for(i=0; i < faces_box.size(); i++){
			face_id ++;
			cv::Mat aligned;
			face_box box = faces_box[i];
			get_aligned_face(img, (float *)&box.landmark, 5, 128, aligned);
		/* extract feature */	
			p_extractor->extract_feature(aligned, info.p_feature);
			if(face_id < UNKNOWN_FACE_ID_MAX)
				info.face_id=get_new_registry_id();
			else
				info.face_id=face_id;
			
			info.name = face_name;
			info.feature_len = 256;

			/* insert feature into mem db */
			std::cout<< "reg: " << info.name << std::endl;
			p_mem_store->insert_new_record(info);

			/* insert feature into verifier */

			p_verifier->insert_feature(info.p_feature,info.face_id); 
		}
	}

}

static void  exec_register_face_feature(shell_cmd_para * p_para)
{
	unsigned int face_id=p_para->face_id;
	unsigned int i;
	face_window * p_win;

	for(i=0;i<face_win_list.size();i++)
	{
		if(face_win_list[i]->face_id == face_id &&
				face_win_list[i]->frame_seq == current_frame_count)
			break;
	}

	if(i==face_win_list.size())
	{
		std::cout<<"cannot find face with id: "<<face_id<<std::endl;
		return;
	}

	p_win=face_win_list[i];

	/* extract feature first */

	face_info info;

	info.p_feature=(float *)malloc(256*sizeof(float));

	cv::Mat aligned;

	/* align face */
	get_aligned_face(*p_cur_frame,(float *)&p_win->box.landmark,5,128,aligned);

	/* get feature */
	p_extractor->extract_feature(aligned,info.p_feature);

        if(face_id<UNKNOWN_FACE_ID_MAX)
		info.face_id=get_new_registry_id();
	else
		info.face_id=face_id;

	info.name=p_para->name;
	info.feature_len=256;

	/* insert feature into mem db */

	p_mem_store->insert_new_record(info);

	/* insert feature into verifier */

	p_verifier->insert_feature(info.p_feature,info.face_id);    

}

void register_face_feature(int argc, char * argv[])
{
	int ret;
	char * username=NULL;
	int face_id=-1;

	optind=1;

	while((ret=getopt(argc,argv,"i:u:"))!=-1)
	{
		switch(ret)
		{
			case 'i':
				face_id=strtoul(optarg,NULL,10);
				break;
			case 'u':
				username=optarg;
				break;
			default:
				break;
		}
	}

	if(face_id<0 || username==NULL)
	{
		fprintf(stdout,"bad arguments\n");
		return ;
	}

	/* check if face_id is a registered one */

	face_info * p_info=p_mem_store->find_record(face_id);

	if(p_info && p_info->name != username)
	{
		fprintf(stdout,"do not support change name from %s to %s\n",
				p_info->name.c_str(),username);
		return ;
        }

	/* setup command para */

	shell_cmd_para * p_para=get_shell_cmd_para();

	p_para->op="reg";
	p_para->face_id=face_id;
	p_para->name=username;

	mem_sync;

	p_para->cmd_status=CMD_STATUS_PENDING;

}
/* list registered faces */

void list_registered_face_info(int argc, char * argv[])
{
	std::vector<face_info *> list;
	int n=p_mem_store->get_all_records(list);

	for(int i=0;i<n;i++)
	{
		face_info * p_info=list[i];

		printf("%-2d\t%d\t%s\n",i,p_info->face_id,p_info->name.c_str());
	}

	std::cout<<"total "<<n<<" faces registered"<<std::endl;
}

/* remove face feature */

void delete_face_feature(int argc, char * argv[])
{
	int ret;
	int face_id=-1;
	char * username=NULL;
	optind=1;

	while((ret=getopt(argc,argv,"i:u:"))!=-1)
	{
		switch(ret)
		{
			case 'i':
				face_id=strtoul(optarg,NULL,10);
				break;
			case 'u':
				username=optarg;
				break;
			default:
				break;
		}
	}

	if(face_id>=0 && username!=NULL)
	{
		fprintf(stdout,"cannot set face_id and name both at one time\n");
		return ;    
	}

	if((face_id<0) && (username==NULL))
	{
		fprintf(stdout,"bad arguments\n");
		return ;
	}

	/* setup cmd para */

	/* setup command para */

	shell_cmd_para * p_para=get_shell_cmd_para();

	p_para->op="park";

	mem_sync;

	p_para->cmd_status=CMD_STATUS_PENDING;

	while(p_para->cmd_status!=CMD_STATUS_RUN);
	/* cv thread is parking now */

	std::vector<face_info *> list;

	if(username)
	{
		p_mem_store->find_record(username,list);
	}
	else
	{
		face_info  * p=p_mem_store->find_record(face_id);

		if(p!=nullptr)
			list.push_back(p);
	}

	if(list.size()==0)
	{
		std::cout<<"No target face found"<<std::endl;
	}

	for(int i=0;i<list.size();i++)
	{
		face_info * p=list[i];
		face_id=p->face_id;

		p_verifier->remove_feature(face_id);
		p_mem_store->remove_record(face_id);

		/* change the name in face_win_list to unknown */

		for(int l=0;l<face_win_list.size();l++)
		{
			face_window * p_win=face_win_list[l];

			if(p_win->face_id==face_id)
			{
				p_win->face_id=get_new_unknown_face_id();
                                p_win->name=" ";
				sprintf(p_win->title,"%d %s",p_win->face_id,p_win->name.c_str());
			}
		}

	}

	std::cout<<"total "<<list.size()<<" face/feature deleted"<<std::endl;


	p_para->cmd_status=CMD_STATUS_DONE;
}

void change_face_id_name(int argc, char * argv[])
{
	int ret;
	optind=1;
	int face_id=-1;
	char * username=NULL;

	while((ret=getopt(argc,argv,"i:u:"))!=-1)
	{
		switch(ret)
		{
			case 'i':
				face_id=strtoul(optarg,NULL,10);
				break;
			case 'u':
				username=optarg;
				break;
			default:
				break;
		}

	}

	if(face_id<0 || username==NULL)
	{
		fprintf(stdout,"bad arguments\n");
		return ;
	}

	/* check if face_id is a registered one */

	face_info * p_info=p_mem_store->find_record(face_id);

	if(p_info ==nullptr)
	{
		fprintf(stdout,"No such face id: %d\n",face_id);
		return ;
	}

	if(p_info->name == username)
	{
		fprintf(stdout,"Nothing needs to do\n");
		return ;
	}

	/* setup command para */

	shell_cmd_para * p_para=get_shell_cmd_para();
	p_para->op="park";
	mem_sync;
	p_para->cmd_status=CMD_STATUS_PENDING;

	while(p_para->cmd_status!=CMD_STATUS_RUN);

	p_info->name=username;

	/* update win */
	for(int i=0;i<face_win_list.size();i++)
	{
		face_window * p_win=face_win_list[i];

		if(p_win->face_id == face_id)
		{
			p_win->name=p_info->name;
		}
	}

	p_para->cmd_status=CMD_STATUS_DONE;
}


void park_cv_thread(shell_cmd_para * p_para)
{
	while(p_para->cmd_status!=CMD_STATUS_DONE)
	{
		asm volatile("":::"memory");
	}

}

void exit_face_demo(int argc, char * argv[])
{
	/* it is too rude ... */
	quit_flag=1;
}

void set_max_face_number(int argc, char * argv[])
{
	int ret;
	optind=1;
	unsigned int maxnum=3;
	
	
while((ret=getopt(argc,argv,"i:"))!=-1)
	{
            switch(ret)
            {
                case 'i':
                        maxnum=strtoul(optarg,NULL,10);
                        std::cout << "optarg:" << optarg << std::endl;
                        break;
                default:
                        break;
            }

	}
        maxFaceNum=maxnum;
        std::cout << "Set up max face number: " << maxnum << std::endl;
}

void init_shell_cmd(void)
{
	/* this is for command executed in cv thread */
	register_shell_executor("reg", exec_register_face_feature);
	register_shell_executor("park", park_cv_thread);

	/* this for command executed in net shell thread */
	register_network_shell_cmd("reg",register_face_feature,"reg -i face_id -u name","register/update a face feature into system");

	register_network_shell_cmd("list",list_registered_face_info,"list","display info of all registered faces");

	register_network_shell_cmd("del",delete_face_feature,"del {-i face_id|-u name}","delete face features by face id or by name");

	register_network_shell_cmd("rename",change_face_id_name,"rename -i face_id -u new_name","rename the name of face feature by id");

	register_network_shell_cmd("exit",exit_face_demo,"exit","exit the demo");

        register_network_shell_cmd("maxfn",set_max_face_number,"maxfn -i maxFaceNum","set the max face number to verify");

}

/***********************************************************************************/

void get_face_name_by_id(unsigned int face_id, std::string& name)
{
	face_info * p_info;

	p_info=p_mem_store->find_record(face_id);

	if(p_info==nullptr)
	{
		name="nullname";
	}
	else
	{
		name=p_info->name;
	}
}


void sig_user_interrupt(int sig, siginfo_t * info, void * arg)
{
	std::cout<<"User interrupt the program ...\n"<<std::endl;
	quit_flag=1;
}


void drop_aged_win(unsigned int frame_count)
{
	std::vector<face_window *>::iterator it=face_win_list.begin();

	while(it!=face_win_list.end())
	{
                if((*it)->frame_seq+win_keep_limit<frame_count)
		{
			delete (*it);
			face_win_list.erase(it);
		}
		else
			it++;
	}
}

face_window * get_face_id_name_by_position(face_box& box,unsigned int frame_seq)
{
	int found=0;
	float center_x=(box.x0+box.x1)/2;
	float center_y=(box.y0+box.y1)/2;
	face_window * p_win;

	std::vector<face_window *>::iterator it=face_win_list.begin();

	while (it!=face_win_list.end())
	{
		p_win=(*it);
		float offset_x=p_win->center_x-center_x;
		float offset_y=p_win->center_y-center_y;

                if((offset_x<trace_pixels) &&
                                (offset_x>-trace_pixels) &&
                                (offset_y<trace_pixels) &&
                                (offset_y>-trace_pixels) &&
                                (p_win->frame_seq+win_keep_limit)>=frame_seq)
		{
			found=1;
			break;
		}
		it++;
	}


	if(!found)
	{
		p_win=new face_window();
                p_win->name=" ";
		p_win->face_id=get_new_unknown_face_id();
	}

	p_win->box=box;
	p_win->center_x=(box.x0+box.x1)/2;
	p_win->center_y=(box.y0+box.y1)/2;
	p_win->frame_seq=frame_seq;

	if(!found)
		face_win_list.push_back(p_win);

	return  p_win;

}

void get_face_title(cv::Mat& frame,face_box& box,unsigned int frame_seq)
{
	float feature[256];
	int face_id;
	float score;
	face_window * p_win;
	float score_thresh = 0.7;

	p_win=get_face_id_name_by_position(box,frame_seq);

#if 1
	cv::Mat aligned;

	/* align face */
        /*int ret_ali = get_aligned_face(frame,(float *)&box.landmark,5,128,aligned);*/
        get_aligned_face(frame,(float *)&box.landmark,5,128,aligned);
	/* get feature */
	p_extractor->extract_feature(aligned,feature);

	/* search feature in db */

	int ret=p_verifier->search(feature,&face_id,&score);

	p_win->add_score(score);

        /*float avg_score = p_win->get_avg_score();*/

	/* found in db*/
	if(ret==0 && score>score_thresh)
	{
		p_win->face_id=face_id;
		get_face_name_by_id(face_id,p_win->name);
	}
        else if(p_win->name != " ")
	{
                p_win->name=" ";
		p_win->face_id=get_new_unknown_face_id();
	}	
#endif
	/*std::cout<<"face_demo: ret" << ret_ali << ", score " << score << ", avg_score " << avg_score << " score_thresh " << score_thresh << std::endl;*/
	sprintf(p_win->title,"%d %s",p_win->face_id,p_win->name.c_str());
}

void draw_box_and_title(cv::Mat& frame, face_box& box, char * title)
{

	float left,top;

	left=box.x0;
	top=box.y0-10;

	if(top<0)
	{
		top=box.y1+20;
	}

	cv::putText(frame,title,cv::Point(left,top),CV_FONT_HERSHEY_PLAIN, 1.5, cv::Scalar(0, 255, 0), 2);

	cv::rectangle(frame, cv::Point(box.x0, box.y0), cv::Point(box.x1, box.y1), cv::Scalar(0, 255, 0), 1);

	for(int l=0;l<5;l++)
	{
		cv::circle(frame,cv::Point(box.landmark.x[l],box.landmark.y[l]),1,cv::Scalar(0, 0, 255),2);
	}

}

void show_params()
{
    std::cout << "\nwin_keep_limit: " << win_keep_limit
              << "\ntrace_pixels: " << trace_pixels
              << "\nUNKNOWN_FACE_ID_MAX: " << UNKNOWN_FACE_ID_MAX
              << "\nmtcnn_min_size: " << mtcnn_min_size
              << "\nmtcnn_pnet_threshold: " << mtcnn_pnet_threshold
              << "\nmtcnn_rnet_threshold: " << mtcnn_rnet_threshold
              << "\nmtcnn_onet_threshold: " << mtcnn_onet_threshold
              << "\nmtcnn_factor: " << mtcnn_factor
              << "\nmaxFaceNum: " << maxFaceNum
              << "\nver_delay_frame: " << ver_delay_frame
			  << "\nknown_faces_path: " << known_faces_path
              << "\nRead complete!" << std::endl;
}

void default_config()
{
    std::cout << "Parse_config error, using default parameters.\n";

    win_keep_limit=10;
    trace_pixels=100;
    UNKNOWN_FACE_ID_MAX=1000;

    mtcnn_min_size=48;
    mtcnn_pnet_threshold=0.9;
    mtcnn_rnet_threshold=0.9;
    mtcnn_onet_threshold=0.9;
    mtcnn_factor=0.6;

    maxFaceNum=3;
    ver_delay_frame=12;

	known_faces_path="./konwn_faces";

    show_params();
}

bool parse_config(const char * path)
{
    std::ifstream ifs;
    Json::Reader reader;
    Json::Value root;

    ifs.open(path, std::ios::in | std::ios::binary);
    if (ifs.is_open() == false) {
                std::cout << "Open json file failed!\n";
        return false;
    }

    if (!reader.parse(ifs, root, false)) {
                std::cout << "Read json file failed!\n";
        ifs.close();
        return false;
    }

    win_keep_limit = root["win_keep_limit"].asInt();
    trace_pixels = root["trace_pixels"].asInt();
    UNKNOWN_FACE_ID_MAX = root["UNKNOWN_FACE_ID_MAX"].asInt();

    mtcnn_min_size = root["mtcnn_min_size"].asFloat();
    mtcnn_pnet_threshold = root["mtcnn_pnet_threshold"].asFloat();
    mtcnn_rnet_threshold = root["mtcnn_rnet_threshold"].asFloat();
    mtcnn_onet_threshold = root["mtcnn_onet_threshold"].asFloat();
    mtcnn_factor = root["mtcnn_factor"].asFloat();

    maxFaceNum = root["maxFaceNum"].asInt();
    ver_delay_frame = root["ver_delay_frame"].asInt();

	known_faces_path = root["known_faces_path"].asString();

    ifs.close();
    return true;
}

bool GreaterSort (face_box a,face_box b) { return (abs(a.x1-a.x0)*(a.y1-a.y0) > abs((b.x1-b.x0)*(b.y1-b.y0))); }

int main(int argc, char * argv[])
{
	::google::InitGoogleLogging(argv[0]);
	const char * type="caffe";
	struct  sigaction sa;

	int res;

	while((res=getopt(argc,argv,"f:t:s"))!=-1)
	{
		switch(res)
		{
			case 't':
				type=optarg;
				break;
			default:
				break;
		}
	}

	sa.sa_sigaction=sig_user_interrupt;
	sa.sa_flags=SA_SIGINFO;
	sigemptyset(&sa.sa_mask);

	sigaction(SIGTERM,&sa,NULL);
	sigaction(SIGINT,&sa,NULL);

	std::string model_dir=MODEL_DIR;

        /* load parameters from json. */
        std::string config_filename = "./models/face_demo.json";
        if(parse_config(config_filename.data()))
        {
            std::cout << "Parse_config from json file: \n";
            show_params();
        }
        else
        {
            default_config();
        }

	mtcnn * p_mtcnn=mtcnn_factory::create_detector(type);

	if(p_mtcnn==nullptr)
	{
		std::cerr<<type<<" is not supported"<<std::endl;
		std::cerr<<"supported types: ";
		std::vector<std::string> type_list=mtcnn_factory::list();

		for(int i=0;i<type_list.size();i++)
			std::cerr<<" "<<type_list[i];

		std::cerr<<std::endl;

		return 1;
	}

	p_mtcnn->load_model(model_dir);
        p_mtcnn->set_threshold(mtcnn_pnet_threshold,mtcnn_rnet_threshold,mtcnn_onet_threshold);
        p_mtcnn->set_factor_min_size(mtcnn_factor,mtcnn_min_size);

	/* alignment */

	/* extractor */

	const std::string extractor_name("lightened_cnn");

	p_extractor=extractor_factory::create_feature_extractor(extractor_name);

	if(p_extractor==nullptr)
	{
		std::cerr<<"create feature extractor: "<<extractor_name<<" failed."<<std::endl;

		return 2;
	}

	p_extractor->load_model(model_dir);

	/* verifier*/

	p_verifier=get_face_verifier("cosine_distance");
	p_verifier->set_feature_len(p_extractor->get_feature_length());

	/* store */

	p_mem_store=new face_mem_store(256,10);

	shell_cmd_para * p_para=get_shell_cmd_para();

	init_network_shell();
	init_shell_cmd();
	create_network_shell_thread("face>",8080);


	cv::VideoCapture camera;

	camera.open(0);

	if(!camera.isOpened())
	{
		std::cerr<<"failed to open camera"<<std::endl;
		return 1;
	}

	cv::Mat frame;


	while(!quit_flag)
	{
            std::vector<face_box> face_info;

            camera.read(frame);

            current_frame_count++;

            /*unsigned long start_time=get_cur_time();*/

            p_mtcnn->detect(frame,face_info);
	        	
            /* filter the faces in face_info, up to maxFaceNum faces left. */
			
            if(face_info.size()>maxFaceNum)
            {
		std::sort(face_info.begin(),face_info.end(),GreaterSort);
                face_info.erase(face_info.begin()+maxFaceNum,face_info.end());
            }
               
            if(current_frame_count % ver_delay_frame)
            {
                for(unsigned int i=0;i<face_info.size();i++)
                {
                    face_window * p_win=get_face_id_name_by_position(face_info[i],current_frame_count);
                    sprintf(p_win->title,"%d %s",p_win->face_id,p_win->name.c_str());
                    draw_box_and_title(frame,face_info[i],p_win->title);
                }
            }
            else
            {
                for(unsigned int i=0;i<face_info.size();i++)
                {
                    face_box& box=face_info[i];
                    get_face_title(frame,box,current_frame_count);
                }

                if(p_para->cmd_status==CMD_STATUS_PENDING)
                {
                    mem_sync;
                    p_cur_frame=&frame;
                    execute_shell_command(p_para);
                }

                for(unsigned int i=0;i<face_win_list.size();i++)
                {
                    if(face_win_list[i]->frame_seq!= current_frame_count)
                        continue;
                    draw_box_and_title(frame,face_win_list[i]->box,face_win_list[i]->title);
                }

                drop_aged_win(current_frame_count);
            }

                /*unsigned long end_time=get_cur_time();*/

                /*std::cerr<<"total detected: "<<face_info.size()<<" faces. used "<<(end_time-start_time)<<" us"<<std::endl;*/

		cv::imshow("camera",frame);

		cv::waitKey(1);

	}

	return 0;
}


