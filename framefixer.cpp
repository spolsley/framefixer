/***************************************************************
/ framefixer
****************************************************************
/ Description:
/ Prepares high fps video for downsampling; framefixer searches
/ for duplicate frames in lower fps content and attempts to
/ adjust spacing of duplicate frames for optimal downsampling
/ (e.g. a game running at 30fps in 60fps video can downsample
/ with no frame loss if each is present exactly 2 times)
/
/ Author: Seth Polsley
***************************************************************/

#include <opencv2/opencv.hpp>
#include <iostream>
#include <iomanip>
#include <list>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <signal.h> // POSIX specific code will be used for ctrl-c handling

using namespace std;
using namespace cv;

// Struct to hold the frames and associated info
struct Frame {
	Mat data;
	Mat comp;
	int count = 0;
	double priority = 0.0;
	int index; // track the frame's original index when read to keep writing index within bounds
};

// Threshold will have high and low settings
// "strict" requires very large changes before it will consider the frame as new
// "relaxed" allows the frame to be considered new with much less overall change
// best to start with strict and relax as more duplicates appear
// e.g. ensure main changes are captured at first, then save any possible as more spots open
class Threshold {
public:
	double value = 0.5; // first instantiate with the default but can change in args
	double strict = 0.5;
	double relaxed = 0.25;
	void makeStrict() {
		value = strict;
	}
	void makeRelaxed() {
		value = relaxed;
	}
};

// Global definitions, using globals for speed
VideoCapture CAP;
VideoWriter VIDEO;
double FPS; // actually should be double because even at 60 fps, video could be fractional (e.g. 59.96 fps)
Threshold THRESH; // starts on strict
int WRITE_INDEX = 0; // global index counter to track progress
int COMP_WIDTH;
int COMP_HEIGHT;
chrono::time_point<chrono::system_clock> START;
chrono::time_point<chrono::system_clock> RUNNING;
double LAST_FPS = 0.0, LAST_SPEED = 0.0; // not a true moving average, but average reporting with last to keep some form of stability
int LAST_INDEX = 0; // additional index tracking for reading and reporting
int READ_INDEX = -1; // starts at -1 since 0-based (first frame is actually 0)
int TOTAL_LENGTH;
bool FINISHED = false;
int DRIFT = 0; // used to manage adjustment bounds

// Catching ctrl-c allows program to stop and write current progress
void signal_handler(int s) {
	FINISHED = true;
	cout << "Finished writing " << WRITE_INDEX << " frames, quitting..." << endl;
	CAP.release();
	VIDEO.release();
	exit(1);
}

// Frame matching algorithm, rely on standard deviation at the moment, although a variety of methods
bool matchFrames(const Mat& a, const Mat& b, double& stdev) {
	// Primary method for frame comparison
	// Setup
	Mat diff, mean, std;
	// Get absolute difference between frames
	absdiff(a,b,diff);
	// Average the total change and get the standard deviation over the entire frame
	// Standard deviation is an excellent measure of the intensity of differences between the frames
	meanStdDev(diff,mean,std);
	// Save standard deviation to stdev for caller to use to determine frame similarity
	stdev = std.at<double>(0);
	// Return bool representing decision of match (true if they match, false if not a match)
	if (stdev < THRESH.value) {
		return true;
	} else {
		return false;
	}
}

// Writes a certain frame a specified number of times, increments global index counter
void writeFrames(VideoWriter& vidout, const Mat& frame, int& count) {
	// write current frame as many times as specified
	while (count > 0) {
		vidout.write(frame); WRITE_INDEX++;
		count--;
	}
}

// Read frame helper, writes into frame passed-by reference and returns true if read, false if not
bool readFrame(VideoCapture& vidin, Mat& frame, Mat& comp) {
	vidin >> frame; READ_INDEX++;
	if (frame.empty()) {
		return false;
	} else {
		Mat temp;
		cvtColor(frame,temp,COLOR_BGR2GRAY);
		resize(temp,comp,Size(COMP_WIDTH,COMP_HEIGHT),0,0,INTER_NEAREST);
		return true;
	}
}

void timeReporting() {
	// Read relevant values
	// check current index: the one operation from the other thread but read-only
	int current_index = READ_INDEX; // could use read or write index, but read should always show progress
	// check current time: only touched by this thread, so certainly no problems
	chrono::time_point<chrono::system_clock> current_time = chrono::system_clock::now();
	
	// Calculations
	int frames = current_index - LAST_INDEX;
	chrono::duration<float> time_duration = current_time - RUNNING;
	float time_difference = time_duration.count();

	chrono::duration<float> global_duration = current_time - START;
	float global_difference = global_duration.count();
	
	double new_fps = (frames/time_difference + LAST_FPS)/2;
	double new_speed = (frames/(time_difference*FPS) + LAST_SPEED)/2;

	// Reporting
	cout << "frame= " << current_index << "  "
		<< "fps= " << new_fps << "  "
		<< "time= " << current_index/FPS << "s  "
		<< "speed= " << new_speed << "x  "
		<< "total= " << 100.0*current_index/TOTAL_LENGTH << "%  " 
		<< "runtime= " << global_difference << "s" << endl;
	
	// Update tracking
	LAST_FPS = new_fps;
	LAST_SPEED = new_speed;

	LAST_INDEX = current_index;
	RUNNING = current_time;
}

void timeReportingManager() {
	START = chrono::system_clock::now();
	RUNNING = START; // running timer tied with start timer at start
	while(!FINISHED) {
		this_thread::sleep_for(chrono::seconds(1));
		timeReporting();
	}
	chrono::time_point<chrono::system_clock> current_time = chrono::system_clock::now();
	chrono::duration<float> time_duration = current_time - START;
	float time_difference = time_duration.count();
	
	cout << TOTAL_LENGTH << " frames processed in " << time_difference << " seconds" << endl;
}

void printUsage() {
	cout << "usage: framefixer <input> <output> [options]" << endl
		<< "  options:" << endl
		<< "    -buffer_size <integer>" << endl
		<< "      distinct frames considered when adjusting; default is 7" << endl
		<< "    -comparison_scale <integer>" << endl
		<< "      factor by which to reduce frames for matching; default is 4, disable with 1" << endl
		<< "    -adjustment_bound <integer>" << endl
		<< "      helps ensure audio stays synced by bounding adjustment distance; default is 5" << endl
		<< "    -duplicate_count <integer>" << endl
		<< "      number of times a frame should repeat to avoid being lost; default is 2" << endl
		<< "    -threshold_strict <float>" << endl
		<< "      standard deviation threshold to use when matching frames; default is 0.5" << endl
		<< "    -threshold_relaxed <float>" << endl
		<< "      relaxed comparison threshold; default is strict/2, disable with equal to strict" << endl;
}

// Main body
int main(int argc, char* argv[]) {
	
	// Argument handling
	if (argc < 3) {
		printUsage();
		return 1;
	}
	
	string input, output;
	int buffer_size = 7;
	int comparison_scale = 4;
	int adjustment_bound = 5;
	int duplicate_count = 2;
	double threshold_strict = -1, threshold_relaxed = -1;
	
	input = argv[1];
	output = argv[2];
	
	if (argc > 3) {
		string arg;
		double val; // use double to get threshold values less than 1, all other args just truncate to int anyways
		try {
			for (int i = 3; i < argc; i++) {
				arg = argv[i];
				sscanf(argv[++i],"%lf",&val); // increment i and read val; goes to catch() if args not passed this way
				if (val <= 0) {
					cout << "all args must be positive values, using default value for " << arg << endl;
				}
				else {
					// could use char-based args and then switch, but I prefer readable string args
					// just use python-esque solution of chaining if/elseif to avoid another library
					if (arg == "-buffer_size") buffer_size = val;
					else if (arg == "-comparison_scale") comparison_scale = val;
					else if (arg == "-adjustment_bound") adjustment_bound = val;
					else if (arg == "-duplicate_count") duplicate_count = val;
					else if (arg == "-threshold_strict") threshold_strict = val;
					else if (arg == "-threshold_relaxed") threshold_relaxed = val;
					else cout << "unrecognized argument " << arg << ", ignoring..." << endl;
				}
			}
		} catch (...) {
			cout << "Unable to parse arguments, quitting..." << endl;
			printUsage();
			return 1;
		}
	}

	// Update global threshold if necessary
	if (threshold_strict > 0) {
		THRESH.strict = threshold_strict;
		THRESH.value = threshold_strict; // start current value on strict
		if (threshold_relaxed > 0) {
			THRESH.relaxed = threshold_relaxed;
		} else {
			THRESH.relaxed = 0.5*threshold_strict; // default is half of strict if not specified
		}
	}
	
	// Video input setup
	// Create a VideoCapture object and open the input file (string name for file, 0 for webcam)
	CAP.open(input);
	
	// Check if VideoCapture opened successfully
	if(!CAP.isOpened())
	{
		cout << "Error opening video stream, quitting..." << endl;
		return -1;
	}
	
	// Copy properties
	// Default resolution of the frame is obtained. The default resolution is system dependent.
	int frame_width = CAP.get(CAP_PROP_FRAME_WIDTH);
	int frame_height = CAP.get(CAP_PROP_FRAME_HEIGHT);
	
	// Comparison sizes
	COMP_WIDTH = frame_width/comparison_scale;
	COMP_HEIGHT = frame_height/comparison_scale;
	
	// Match fps of input on output
	FPS = CAP.get(CAP_PROP_FPS);
	TOTAL_LENGTH = CAP.get(CAP_PROP_FRAME_COUNT);
	
	// Match codec by getting fourcc code
	int fcc = CAP.get(CAP_PROP_FOURCC);
	string fcc_s = format("%c%c%c%c", fcc & 255, (fcc >> 8) & 255, (fcc >> 16) & 255, (fcc >> 24) & 255);
	
	// Video output setup
	// Use provided name and copied properties; should match input exactly with adjusted frames
	VIDEO.open(output,VideoWriter::fourcc(fcc_s[0],fcc_s[1],fcc_s[2],fcc_s[3]),FPS,Size(frame_width,frame_height));
	
	cout << std::fixed;
	cout << std::setprecision(2);

	// Initial reporting
	cout << "Input: " << input << endl
		<< "Output: " << output << endl
		<< "Length: " << TOTAL_LENGTH/FPS << "s, "
		<< "Frames: " << TOTAL_LENGTH << ", "
		<< "Fps: " << FPS << ", "
		<< "Dimensions: " << frame_width << "x" << frame_height  << ", "
		<< "Codec: " << fcc_s << endl;

	cout << "Settings: " << endl
		<< "buffer_size=" << buffer_size << ", "
		<< "comparison_scale=" << comparison_scale << ", "
		<< "adjustment_bound=" << adjustment_bound << ", "
		<< "duplicate_count=" << duplicate_count << ", "
		<< "threshold_strict=" << THRESH.strict << ", "
		<< "threshold_relaxed=" << THRESH.relaxed << endl;

	// Start timer
	thread(timeReportingManager).detach();
	
	// Setup signal handling
	struct sigaction sigIntHandler;
	sigIntHandler.sa_handler = signal_handler;
	sigemptyset(&sigIntHandler.sa_mask);
	sigIntHandler.sa_flags = 0;
	sigaction(SIGINT,&sigIntHandler,NULL);

	// Prepare for main loop
	list<Frame*> buffer;
	
	Mat tempframe, compframe;
	double stdev = 0.0;
	bool full = false; // ensures buffer doesn't overflow
	bool fixing = true; // flag to track if fixing of frame is possible/happening
	int drift_update = 0; // counter will manage drift updating, will get reset to buffer or half buffer
	
	if (readFrame(CAP,tempframe,compframe)) {
		// must read first frame for comparison and setup initial count
		// could put .empty() check in matchFrames but that slows down all frame checking
		Frame* temp = new Frame();
		tempframe.copyTo(temp->data);
		compframe.copyTo(temp->comp);
		temp->count = 1;
		temp->priority = stdev;
		temp->index = READ_INDEX;
		buffer.push_back(temp);
		
		// Main loop goes frame-by-frame, checking match levels, filling buffer, and adjusting
		while(!FINISHED) {
			while(!full) { // this part will continue until the buffer is full
				if (readFrame(CAP,tempframe,compframe)) { // read frame-by-frame
					if (matchFrames(buffer.back()->comp,compframe,stdev)) { // check match
						buffer.back()->count++; // increment duplicate count if a match
						if (buffer.back()->count == duplicate_count) { // relax if goal reached
							THRESH.makeRelaxed();
						}
					} else {
						THRESH.makeStrict(); // always set back to strict when new frame
						if (buffer.size() < buffer_size) {
							Frame* temp = new Frame();
							tempframe.copyTo(temp->data);
							compframe.copyTo(temp->comp);
							temp->count = 1;
							temp->priority = stdev;
							temp->index = READ_INDEX;
							buffer.push_back(temp);
						} else {
							full = true;
						}
					}
				} else {
					full = true; // readFrame failed! probably end of file, so nothing more to fill
					FINISHED = true;
				}
			}
			drift_update--;
			if (drift_update <= 0) {
				DRIFT = WRITE_INDEX - buffer.front()->index;
				drift_update = buffer_size; // only need to recheck after this buffer cleared since DRIFT is updated below
			}
			if (abs(DRIFT) < adjustment_bound) {
				// adjustment phase, going one block at a time to try to fix potential lost frames
				// adjustment could happen anywhere in the buffer, but will adjust frame in middle (still write from front, read into end)
				// could easily use buffer.front() or buffer.back() or a pointer to any generic spot since the code below tries to fix using non-current frame regardless
				list<Frame*>::iterator iter = buffer.begin();
				advance(iter, buffer_size/2);
				Frame* tofix = *iter;
				fixing = true; // necessary to avoid infinite loop with dup adjusting
				while (fixing && tofix->count < duplicate_count) {
					fixing = false; // will do one step of frame adjustment each loop
					// first, see if any other slot can offer this frame a place without risk of loss
					for (list<Frame*>::reverse_iterator it = buffer.rbegin(); it != buffer.rend(); it++) {
						if ((*it)->count > duplicate_count) { // no need to waste time checking *it == tofix; couldn't have entered this loop if tofix->count > dup count
							(*it)->count--;
							tofix->count++;
							fixing = true;
							break;
						}
					}
					// if not, check priority and take a slot from a lower priority frame if need be
					if (!fixing) {
						for (list<Frame*>::reverse_iterator it = buffer.rbegin(); it != buffer.rend(); it++) {
							if ((*it)->priority < tofix->priority && (*it)->count > 1) { // enforce that frames not allowed to be dropped with count > 1
								(*it)->count--;
								tofix->count++;
								fixing = true;
								break;
							}
						}
					}
				}
			} else if (DRIFT >= adjustment_bound) {
				// over bound, so need to cut frames to correct drift
				// go through the buffer, as long as drift is still too high, try to cut frames not at-risk
				for (list<Frame*>::iterator it = buffer.begin(); DRIFT >= adjustment_bound && it != buffer.end(); it++) {
					while ((*it)->count > duplicate_count) { // can shave off copies of current frame
						(*it)->count--;
						DRIFT--;
						if (DRIFT < adjustment_bound) break; // can stop correcting drift
					}
				}
				// have gone through buffer; if drift within bounds, will start adjusting again next round; if not, will keep trying to correct drift next round
			} else {
				// must be under bound, so need to add frames
				// in this case, go through and add to at-risk frames from front to back
				for (list<Frame*>::iterator it = buffer.begin(); abs(DRIFT) >= adjustment_bound && it != buffer.end(); it++) {
					while ((*it)->count < duplicate_count) { // could add to here since at-risk already
						(*it)->count++;
						DRIFT++;
						if (abs(DRIFT) < adjustment_bound) break; // can stop correcting drift
					}
				}
			}
			// write first frame
			writeFrames(VIDEO,buffer.front()->data,buffer.front()->count);
			delete buffer.front(); // free memory of Frame object
			buffer.pop_front(); // clear record from list
			full = false;
			// save the last new frame written into tempframe
			Frame* temp = new Frame();
			tempframe.copyTo(temp->data);
			compframe.copyTo(temp->comp);
			temp->count = 1;
			temp->priority = stdev;
			temp->index = READ_INDEX;
			buffer.push_back(temp);
		}
	}
	FINISHED = true;
	
	// Cleanup stage
	// write out any remaining frames and clear buffer
	for (list<Frame*>::iterator it = buffer.begin(); it != buffer.end(); it++) {
		writeFrames(VIDEO,(*it)->data,(*it)->count);
		delete *it; // free memory of Frame object
	}
	buffer.clear(); // clear all records from list
	
	// release video devices
	CAP.release();
	VIDEO.release();
	
	// Closes all the windows
	destroyAllWindows();
	return 0;
	
}
