
#include <iostream>
#include <string>
#include <thread>
extern "C"{
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswresample/swresample.h>
	#include <libavutil/avstring.h>
	#include <libavutil/pixfmt.h>
	#include <libavutil/samplefmt.h>
	#include <libavutil/channel_layout.h>
	#include <libavutil/audio_fifo.h>
	#include <libswscale/swscale.h>
	#include <windows.h>
	#include <process.h>
}
using namespace std;

bool       rtsp_state=false;
int        pts_rtsp=0;
int audio_frame_index_rtsp=0;
int video_frame_index_rtsp=0;

int OpenRtspStream(const char* url,AVFormatContext **ic)
{
	AVDictionary* options = NULL; 
	int ret=-1;
	av_dict_set(&options,"rtsp_transport", "tcp", 0);  	
	av_dict_set(&options,"stimeout","10000000",0);
	if(avformat_open_input(ic,url,NULL,&options)!=0)          //avformat_close_input 关闭
		return -1;
	if(avformat_find_stream_info(*ic,NULL)<0)
		return -1;		
	printf("-----------rtsp流输入信息--------------\n");
	av_dump_format(*ic, 0, url,0);
	printf("---------------------------------------\n");
	printf("\n");
	return 0;
}

int Find_StreamIndex(AVFormatContext* ic,enum AVMediaType type)
{
	int Index=-1;
	for(unsigned int i=0; i< ic->nb_streams; i++)
		if(ic->streams[i]->codecpar->codec_type==type)
		{
			Index=i;
			break;
		}
		if(Index==-1)
			return -1;
		return Index;
}

int Open_Decoder(AVFormatContext** ic,AVCodecContext** pInCodecCtx,int index,bool open) 
{
	AVCodec* pInCodec=NULL;
	int ret=-1;
	pInCodec=avcodec_find_decoder((*ic)->streams[index]->codecpar->codec_id);
    if(pInCodec==NULL)
		return -1;
	*pInCodecCtx=avcodec_alloc_context3(pInCodec);
	if(!(*pInCodecCtx))
		return -1;
	ret=avcodec_parameters_to_context(*pInCodecCtx,(*ic)->streams[index]->codecpar);
	if(ret<0)
		return -1;
	if(open)
	{
		if(avcodec_open2(*pInCodecCtx, pInCodec,NULL)<0)
			return -1;
	}
	return 0;

}

AVStream* AddVideoStream(AVFormatContext* oc,AVCodecContext** pOutCodecCtx,AVFormatContext* ic,int index,bool mark,bool open)
{
	AVCodec   *video_codec=NULL;
	AVStream  *VideoSt=NULL;

	video_codec=avcodec_find_encoder(ic->streams[index]->codecpar->codec_id);
	if(!video_codec)
		return NULL;
	VideoSt=avformat_new_stream(oc,video_codec);
	if (!VideoSt)
	{
		return NULL;
	}
	*pOutCodecCtx=avcodec_alloc_context3(video_codec);
	if(!(*pOutCodecCtx))
		return NULL;


	if(avcodec_parameters_copy(VideoSt->codecpar,ic->streams[index]->codecpar)<0)
		return NULL;
	

	if(avcodec_parameters_to_context(*pOutCodecCtx,VideoSt->codecpar)<0)
		return NULL;

	if(av_q2d(ic->streams[index]->avg_frame_rate)<20.0||av_q2d(ic->streams[index]->avg_frame_rate)>30.0||ic->streams[index]->avg_frame_rate.den==0)
		(*pOutCodecCtx)->time_base.den=25;
	else
		(*pOutCodecCtx)->time_base.den  = av_q2d(ic->streams[index]->avg_frame_rate);//帧率： 30
	VideoSt->time_base.num=1;
	VideoSt->time_base.den=ic->streams[index]->time_base.den;

	if(!mark)
	{
		if(oc->oformat->flags&AVFMT_GLOBALHEADER)
			(*pOutCodecCtx)->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	if(open)
	{
		if(avcodec_open2(*pOutCodecCtx,video_codec,NULL)<0)
			return NULL;
	}
	return VideoSt;
}

AVStream* AddAudioStream(AVFormatContext* oc,AVCodecContext** pOutCodecCtx,AVFormatContext* ic,int index,bool mark,bool open)
{
	AVStream *AudioSt=NULL;
	AVCodec *audio_codec=NULL;
	audio_codec=avcodec_find_encoder(AV_CODEC_ID_AAC);
	if(!audio_codec)
		return NULL;

	AudioSt=avformat_new_stream(oc,audio_codec);
	if (!AudioSt)
		return NULL;

	*pOutCodecCtx=avcodec_alloc_context3(audio_codec);
	if(!(*pOutCodecCtx))
		return NULL;

	(*pOutCodecCtx)->channels=2;
	(*pOutCodecCtx)->channel_layout=av_get_default_channel_layout((*pOutCodecCtx)->channels);
	(*pOutCodecCtx)->sample_rate=ic->streams[index]->codecpar->sample_rate;
	(*pOutCodecCtx)->sample_fmt=audio_codec->sample_fmts[0];
	(*pOutCodecCtx)->bit_rate=64000;

	(*pOutCodecCtx)->strict_std_compliance=FF_COMPLIANCE_EXPERIMENTAL;

	(*pOutCodecCtx)->time_base.num=1;
	(*pOutCodecCtx)->time_base.den=ic->streams[index]->codecpar->sample_rate;

	AudioSt->time_base.num=1;
	AudioSt->time_base.den=ic->streams[index]->codecpar->sample_rate;//是否变化，注意测试   时基在发生改变

	if(!mark)
	{
		if(oc->oformat->flags&AVFMT_GLOBALHEADER)
			(*pOutCodecCtx)->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	if(open)
	{
		if(avcodec_open2(*pOutCodecCtx,audio_codec,NULL)<0)
			return NULL;
	}
	if(avcodec_parameters_from_context(AudioSt->codecpar,(*pOutCodecCtx))<0)
		return NULL;

	return AudioSt;
}

int Search_I_Frame(uint8_t* buf,int size)
{
	int i=0,j=0;
	for(i=size-1;i>=3;i--)
	{
		if(buf[i] == 0x01 && buf[i-1] == 0x00 && buf[i-2] == 0x00 && buf[i-3] == 0x00)
			break;
	}
	j=i-3;
	if(i<3)
		return -1;
	else 
		return j;
}

int Init_Converted_Samples(uint8_t ***converted_input_samples,AVCodecContext *output_codec_context,int frame_size)
{

	if (!(*converted_input_samples =(uint8_t **)calloc(output_codec_context->channels,sizeof(**converted_input_samples)))) 
		return -1;

	if (av_samples_alloc(*converted_input_samples, NULL,output_codec_context->channels,frame_size,output_codec_context->sample_fmt, 0) < 0) 
	{
		av_freep(&(*converted_input_samples)[0]);
		free(*converted_input_samples);
		return -1;
	}
	return 0;
}

int Convert_Samples(const uint8_t **input_data,uint8_t **converted_data, const int frame_size,SwrContext *resample_context)
{
	if (swr_convert(resample_context,converted_data, frame_size,input_data, frame_size) < 0) 
	{
		return -1;
	}
	return 0;
}

int Add_Samples_To_Fifo(AVAudioFifo *fifo,uint8_t **converted_input_samples,const int frame_size)
{
	if (av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size) < 0) 
		return -1;

	if (av_audio_fifo_write(fifo, (void **)converted_input_samples,frame_size) < frame_size) 
		return -1;
	return 0;
}

int Audio_Resampler(AVCodecContext *input_codec_context,AVCodecContext *output_codec_context,AVFrame *input_frame,AVAudioFifo *fifo_buffer)
{
	SwrContext *resample_context=NULL;
	uint8_t **converted_input_samples = NULL;
	int ret=-1;
	const int frame_size=input_frame->nb_samples;
	//初始化
	resample_context = swr_alloc_set_opts(NULL,
											av_get_default_channel_layout(output_codec_context->channels),	//输出通道格式
											output_codec_context->sample_fmt,								//输出采样格式
											output_codec_context->sample_rate,								//输出采样率
											av_get_default_channel_layout(input_codec_context->channels),	//输入通道格式
											input_codec_context->sample_fmt,									//输入采样格式
											input_codec_context->sample_rate,								//输入采样率
											0, NULL);
	if (!resample_context||swr_init(resample_context) < 0) 
		return -1;

	ret=Init_Converted_Samples(&converted_input_samples, output_codec_context, input_frame->nb_samples);
	if(ret<0)
		goto cleanup;

	ret=Convert_Samples((const uint8_t**)input_frame->extended_data, converted_input_samples,input_frame->nb_samples, resample_context);
	if(ret<0)
		goto cleanup;
	
	ret=Add_Samples_To_Fifo(fifo_buffer, converted_input_samples, input_frame->nb_samples);
	if(ret<0)
		goto cleanup;

	ret=0;
cleanup:
	if (converted_input_samples) 
	{
		av_freep(&converted_input_samples[0]);
		free(converted_input_samples);
	}
	swr_free(&resample_context);
	return ret;

}

int Init_Output_Frame(AVFrame **frame,AVCodecContext *output_codec_context,int frame_size)
{
	if (!(*frame = av_frame_alloc())) 
		return -1;

	(*frame)->nb_samples     = frame_size;
	(*frame)->channel_layout = output_codec_context->channel_layout;
	(*frame)->format         = output_codec_context->sample_fmt;
	(*frame)->sample_rate    = output_codec_context->sample_rate;

	if (av_frame_get_buffer(*frame, 0) < 0) 
	{
		av_frame_free(frame);
		return -1;
	}
	return 0;
}

int Encode_Audio_Frame_Rtsp(AVFrame *frame,AVFormatContext *oc,AVStream *audio_st,AVCodecContext *out_codec_ctx,int index,AVFormatContext *ic)
{
	int data_present=0;
	int ret=-1;
	AVPacket output_packet;
	av_init_packet(&output_packet);
	output_packet.data=NULL;
	output_packet.size=0;

	if (frame) 
	{
		frame->pts = pts_rtsp;
		pts_rtsp += frame->nb_samples;
	}

	ret=avcodec_send_frame(out_codec_ctx, frame);
	if (ret == AVERROR_EOF) 
	{
		ret=0;
		goto end;
	}else if(ret<0)
	{
		av_packet_unref(&output_packet);
		return -1;
	}

	ret=avcodec_receive_packet(out_codec_ctx,&output_packet);
	if (ret==AVERROR(EAGAIN))
	{
		ret=0;
		goto end;
	}else if(ret==AVERROR_EOF)
	{
		ret=0;
		goto end;
	}else if(ret<0)
	{
		av_packet_unref(&output_packet);
		return -1;
	}

	output_packet.pts=frame->pts;
	output_packet.pts = av_rescale_q_rnd(output_packet.pts, ic->streams[index]->time_base, audio_st->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
	output_packet.dts=output_packet.pts;
	output_packet.duration = av_rescale_q(output_packet.duration, ic->streams[index]->time_base, audio_st->time_base);
	output_packet.stream_index=index;
	if (av_interleaved_write_frame(oc, &output_packet)< 0) 
	{
		av_packet_unref(&output_packet);
		return -1;
	}
	
end:
	av_packet_unref(&output_packet);
	if(ret<0)
		return ret;
	else 
		return 0;
}

int Load_Encode_And_Write(AVAudioFifo *fifo,AVFormatContext *oc,AVStream *audio_st,AVCodecContext *out_codec_ctx,int index,AVFormatContext *ic)
{

	AVFrame *output_frame;
	int ret=-1;
	const int frame_size = FFMIN(av_audio_fifo_size(fifo),out_codec_ctx->frame_size);
	ret=Init_Output_Frame(&output_frame, out_codec_ctx, frame_size);
	if(ret<0)
		return ret;

	if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) 
	{
		av_frame_free(&output_frame);
		return -1;
	}

	ret=Encode_Audio_Frame_Rtsp(output_frame,oc,audio_st,out_codec_ctx,index,ic);
	if(ret<0)
	{
		av_frame_free(&output_frame);
		return ret;
	}
	
	av_frame_free(&output_frame);
	return 0;
}

int RecordVideoAudio( )
{
	AVFormatContext *ofmt=NULL;
	AVFormatContext *ifmt=NULL;
	AVCodecContext  *in_video_ctx=NULL,*in_audio_ctx=NULL;
	AVCodecContext  *out_video_ctx=NULL,*out_audio_ctx=NULL;
	AVStream		*audio_stream=NULL,*video_stream=NULL;
	AVPacket         packet,pktV;
	AVFrame			*pInFrame=NULL;
	AVAudioFifo		*fifo=NULL;
	AVOutputFormat  *fmt=NULL;
	int				ret=-1;
	int				result=0;
	int				pos=0,size=0;
	int				got_frame=0;
	int             audio_index=-1,video_index=-1;
	int				sps_pps_size=0;
	const char* filename="test.mp4";
	int recordtime=9;
	//char* url="rtsp://admin:admin@123@192.168.1.161:554";
	char* url="rtsp://184.72.239.149/vod/mp4://BigBuckBunny_175k.mov";
	long long		record_Time=recordtime*1000*60;//时间以分钟为单位
	long long		StartToSecond=0;
	long long		EndToSecond=0;
	long long		Totaltime=0;

	ifmt=avformat_alloc_context();
	if(!ifmt)
	{
		ret=-1;
		goto end;
	}
	ret=OpenRtspStream(url,&ifmt);
	if(ret<0)
		goto end;


	video_index=Find_StreamIndex(ifmt,AVMEDIA_TYPE_VIDEO);
	audio_index=Find_StreamIndex(ifmt,AVMEDIA_TYPE_AUDIO);

	if(video_index<0)
	{
		cout<<"没有视频流"<<endl;
		ret=-1;
		goto end;
	}


	
	ret=Open_Decoder(&ifmt,&in_video_ctx,video_index,false);
	if(ret<0)
		goto end;

	
	ret=Open_Decoder(&ifmt,&in_audio_ctx,audio_index,true);
	if(ret<0)
			goto end;


	avformat_alloc_output_context2(&ofmt,NULL,NULL,filename);
	fmt=ofmt->oformat;

	audio_stream=AddAudioStream(ofmt,&out_audio_ctx,ifmt,audio_index,false,true);
	if(!audio_stream)
	{
		ret= -1;
		goto end;
	}


	video_stream=AddVideoStream(ofmt,&out_video_ctx,ifmt,video_index,false,false);
	if(!video_stream)
	{
		ret=-1;
		goto end;
	}
	sps_pps_size=ifmt->streams[video_index]->codecpar->extradata_size;

	for(int i=0;i<sps_pps_size;i++)
		video_stream->codecpar->extradata[i]=ifmt->streams[video_index]->codecpar->extradata[i];
	video_stream->codecpar->extradata_size=sps_pps_size;


	//显示输出文件信息
	printf("-----------输出文件信息--------------\n");
	av_dump_format(ofmt, 0, filename,1);
	printf("---------------------------------------\n");

	if(!(fmt->flags&AVFMT_NOFILE))
	{
		ret=avio_open(&ofmt->pb,filename,AVIO_FLAG_WRITE);
		if(ret<0)
		{
			ret=-1;
			goto end;
		}
	}
		
	ret=avformat_write_header(ofmt,NULL);
	if(ret<0)
	{
		ret=-1;
		goto end;
	}

	pInFrame=av_frame_alloc();   
	if(!pInFrame)
	{
		ret= -1;
		goto end;
	}
	fifo=av_audio_fifo_alloc(out_audio_ctx->sample_fmt,out_audio_ctx->channels,1);
	if(!fifo)
	{
		ret= -1;
		goto end;
	}
	
	const int video_duration=video_stream->time_base.den/out_video_ctx->time_base.den;
	const int output_frame_size=out_audio_ctx->frame_size;
	StartToSecond=GetTickCount();
	rtsp_state=true;
	while(rtsp_state)
	{
		av_init_packet(&packet);
		packet.data=NULL;
		packet.size=0;
		EndToSecond=GetTickCount();
		Totaltime=EndToSecond-StartToSecond;
	
		if(Totaltime>record_Time)
			break;
		ret=av_read_frame(ifmt, &packet) ;
		if(ret>=0)
		{
			if(packet.stream_index==video_index)
			{
				pos=Search_I_Frame(packet.data,packet.size);
				if(pos<0)
				{
					ret=pos;
					av_packet_unref(&packet);
					break;
				}
				size=packet.size-pos;
				av_new_packet(&pktV,size);
				memset(pktV.data,0,size);
				memcpy(pktV.data,packet.data+pos,size);
				pktV.flags=packet.flags;
				//cout<<packet.duration<<endl;
				pktV.pts=video_duration*video_frame_index_rtsp;
				pktV.pts = av_rescale_q_rnd(pktV.pts, ifmt->streams[video_index]->time_base, video_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
				pktV.dts = pktV.pts;
				pktV.duration = av_rescale_q(packet.duration, ifmt->streams[video_index]->time_base, video_stream->time_base);
				pktV.stream_index=video_index;
				pktV.flags |= AV_PKT_FLAG_KEY;
				ret=av_interleaved_write_frame(ofmt, &pktV) ;
				if (ret< 0)
				{
					ret=-1;
					av_packet_unref(&pktV);
					av_packet_unref(&packet);
					break;
				}
				video_frame_index_rtsp++;
				av_packet_unref(&pktV);
			}
			else if(packet.stream_index==audio_index)
			{
				ret=avcodec_send_packet(in_audio_ctx,&packet);
				if(ret<0)
				{
					ret=-1;
					break;
				}
				ret=avcodec_receive_frame(in_audio_ctx,pInFrame);
				if(ret==AVERROR(EAGAIN))	
				{
					continue;
				}
				else if(ret==AVERROR_EOF)
				{
					continue;
				}
				else if(ret<0)
				{
					ret=-1;
					break;
				}

				ret=Audio_Resampler(in_audio_ctx,out_audio_ctx,pInFrame,fifo);
				if(ret<0)		
					goto end;

				if(av_audio_fifo_size(fifo) >= output_frame_size)
				{
					ret=Load_Encode_And_Write(fifo,ofmt,audio_stream,out_audio_ctx,audio_index,ifmt);  
					if(ret<0)
						goto end;
				}
			}
		}
		else
		{
			result=-1;
			break;
		}
		
		av_packet_unref(&packet);
	}
	ret=av_write_trailer(ofmt);
	if(ret<0)
	{
		ret=-1;
	}

end:

	if(in_audio_ctx)  
		avcodec_close(in_audio_ctx);
	if(out_audio_ctx) 
		avcodec_close(out_audio_ctx);

	if(fifo)
		av_audio_fifo_free(fifo);
	if(pInFrame)
		av_frame_free(&pInFrame);

	if(in_video_ctx)
		avcodec_free_context(&in_video_ctx);
	if(in_audio_ctx)
		avcodec_free_context(&in_audio_ctx);

	if(out_video_ctx)
		avcodec_free_context(&out_video_ctx);
	if(out_audio_ctx)
		avcodec_free_context(&out_audio_ctx);

	if (ofmt && !(fmt->flags & AVFMT_NOFILE))
		avio_close(ofmt->pb);

	if(ofmt)
		avformat_free_context(ofmt);

	if(ifmt)
	{
		avformat_close_input(&ifmt);
		avformat_free_context(ifmt);
	}

	if(result<0)
		ret=-2;
	if(ret<0)
	{
		cout<<"录制失败,"<<"ret="<<ret<<endl;
		return ret;
	}
	else 
	{
		cout<<"录制成功";
		return 0;
	}
}

void main()
{
	thread task(RecordVideoAudio);
	task.detach();
	Sleep(10000);
	cout<<"按键停止录制"<<endl;
	getchar();
	rtsp_state=false;
	getchar();
}


