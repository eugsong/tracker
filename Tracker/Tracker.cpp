// Tracker.cpp : main project file.

#include "stdafx.h"
#include "arduino.h"
#include <stdio.h>
#include <iostream>
#include <fstream>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <Windows.h>

using namespace System;
using namespace System::IO::Ports;
using namespace std;
using namespace cv;
using SysString = System::String;

double PCFreq = 0.0;
__int64 CounterStart = 0;

void StartCounter()
{
	LARGE_INTEGER li;
	if (!QueryPerformanceFrequency(&li))
		cout << "QueryPerformanceFrequency failed!\n";

	PCFreq = double(li.QuadPart) / 1000.0;

	QueryPerformanceCounter(&li);
	CounterStart = li.QuadPart;
}
double GetCounter()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	return double(li.QuadPart - CounterStart) / PCFreq;
}

void arduino_tx(SerialPort^ arduino, SysString^ message)
{
	arduino->WriteLine(message);
	return;
}

SysString^ arduino_rx(SerialPort^ arduino, int timeout = SerialPort::InfiniteTimeout)
{
	SysString^ serial_response;
	SysString^ main_response;

	arduino->ReadTimeout = timeout;
	try
	{
		do
		{
			serial_response = arduino->ReadLine();
			if (SysString::IsNullOrEmpty(main_response))
			{
				main_response = serial_response;
			}

		} while (!serial_response->Equals("ok\r") && !serial_response->Contains("error:"));
	}
	catch (TimeoutException^ exception)
	{
		arduino->ReadTimeout = SerialPort::InfiniteTimeout;
		throw exception;
	}

	arduino->ReadTimeout = SerialPort::InfiniteTimeout;
	return main_response;
}
SysString^ arduino_tx_rx(SerialPort^ arduino, SysString^ serial_message)
{
	// Writes message to serial port and returns the response. Clears serial buffer of subsequent "ok" or "errors" (for now)

	// TODO Error handling 

	arduino->WriteLine(serial_message);

	SysString^ serial_response;
	SysString^ mainResponse; // This will be the one actually returned. Won't bother returning "ok"
	do
	{
		serial_response = arduino->ReadLine();
		if (SysString::IsNullOrEmpty(mainResponse))
		{
			mainResponse = serial_response;
		}

	} while (!serial_response->Equals("ok\r") && !serial_response->Contains("error:"));

	return mainResponse;
}

void clear_serial_buffer(SerialPort^ arduino, int timeout = 1)
{
	arduino->ReadTimeout = timeout;
	SysString^ response;
	try
	{
		do
		{
			response = arduino->ReadLine();
		} while (!response->Equals("ok\r") && !response->Contains("error:"));
	}
	catch (TimeoutException^ exception)
	{
		arduino->SerialPort::InfiniteTimeout;
		return;
	}
	arduino->SerialPort::InfiniteTimeout;
	return;
}

int main(array<System::String ^> ^args)
{
	// Set up files for testing and loggin
	ofstream loop_time_file;
	loop_time_file.open("loop_time.txt");
	ofstream data_input_time_file;
	data_input_time_file.open("data_input_time.txt");
	data_input_time_file << "query command time, frame cap time, query respond time, image processing time\n";
	ofstream command_time_file;
	command_time_file.open("command_time.txt");
	ofstream command_file;
	command_file.open("commands.txt");
	ofstream misc_time_file; // I just use this for logging timers that I might want to move around
	misc_time_file.open("misc_times.txt");


	StartCounter(); // For getting timing data
	// Timers for stuff that might need them. Can add more later.
	double move_init_timer = GetCounter();
	double loop_timer = GetCounter(); // This is the main timer to monitor the loop
	double command_timer = GetCounter(); // Log how long it takes to write a move command into the buffer
	double data_input_timer = GetCounter(); // Log how long it takes to grab a camera frame and the motor status
	double misc_timer = GetCounter();
	double query_timer = GetCounter();
	double arduino_command_timer = GetCounter(); // Timer used to send commands to 

	int moves_in_queue = 0;

	// Configure serial comms for Arduino
	SysString^ port_name;
	SysString^ serial_response;
	SysString^ serial_message;

	SerialPort^ arduino;
	int baud_rate = 500000; //115200
	port_name = "com5";
	arduino = gcnew SerialPort(port_name, baud_rate);
	arduino->Open();

	// Define some OpenCV primary colors for convenience
	Scalar color_white = Scalar(255, 255, 255);
	Scalar color_black = Scalar(0, 0, 0);
	Scalar color_red = Scalar(255, 0, 0);
	Scalar color_green = Scalar(0, 255, 0);
	Scalar color_blue = Scalar(0, 0, 255);

	// Set up image capture and processing
	vector<vector<Point>> contours; // This is a vector of vectors, i.e. a vector of contours. You will want to work with each element individually, i.e. one contour at a time.
	vector<Vec4i> hierarchy; // For storing contour hierarchy info (i.e. if certain contours are nested within others). Not sure if it's needed, but hey the info is there.
	Mat capped_frame; // This is the original, unprocessed, captured frame from the camera. 
					 // Keeping it separate from the subsequent processing because sometimes it's useful to have the original.
	Mat processed_frame; // This is the one that will be processed and used to find contours on

	VideoCapture vid_cap(CV_CAP_ANY); // This is sufficient for a single camera setup. Otherwise, it will need to be more specific.
	vid_cap.set(CV_CAP_PROP_FPS, 240); // Need to set the exposure time to appropriate value in Pylon Viewer as well
	vid_cap.set(CV_CAP_PROP_FRAME_HEIGHT, 200); // 200x200 window
	vid_cap.set(CV_CAP_PROP_FRAME_WIDTH, 200); 

	// Disable delay between motor moves
	serial_response = arduino_tx_rx(arduino, "?");
	Console::WriteLine(serial_response);
	serial_response = arduino_tx_rx(arduino, "$1=255");
	Console::WriteLine(serial_response);
	// Set status report mask, $10=2 means only return working position when queried (i.e. mm instead of motor counts), helps free up serial bandwidth
	serial_response = arduino_tx_rx(arduino, "$10=2");
	Console::WriteLine(serial_response);
	serial_response = arduino_tx_rx(arduino, "$120=50"); // X accel
	Console::WriteLine(serial_response);
	serial_response = arduino_tx_rx(arduino, "$121=50"); // Y accel
	Console::WriteLine(serial_response); 
	serial_response = arduino_tx_rx(arduino, "F5000"); // Linear move feedrate
	Console::WriteLine(serial_response);
	serial_response = arduino_tx_rx(arduino, "G91"); // Setting for relative move commands
	clear_serial_buffer(arduino, 1000);

	// Initial jog (dunno why I have to do this right now)
	//arduino_tx_rx(arduino, "G0 X1");
	//move_init_timer = GetCounter();
	//while (GetCounter() - move_init_timer < 2000);
	//arduino_tx_rx(arduino, "G0 X-1");

	// @@@@@@ MAIN LOOP @@@@@@
	bool stream_enabled = false; // Whether or not to display streaming window for testing purposes
								 // If I ever implement multithreading, then I might be able to turn this on permanently
	bool stream_only = false; // Enable just for camera testing without any motion
	bool console_enabled = false; // Enable to allow access to the console to send G-code manually to the Arduino
								  // Streaming will be gimped if it's enabled at the same time (until I enable multithreading)

	boolean is_following = false; // for use when the contour leaves the frame
	boolean ready_to_send_next_move_cmd = true;
	boolean first_query = true;
	while (true)
	{
		while (GetCounter() - query_timer < 4.15)
		{

		}
		loop_time_file << GetCounter() - query_timer << "\n";
		query_timer = GetCounter(); // make sure there is a full period between loops
		if (first_query)
		{
			data_input_timer = GetCounter();
			arduino_tx(arduino, "?"); // Query Grbl status
			data_input_time_file << GetCounter() - data_input_timer << ", ";
		}
		first_query = false;

		data_input_timer = GetCounter();
		vid_cap >> capped_frame;
		if (capped_frame.empty())
		{
			// Frame was dropped
			// TODO Make this more elegant in the final implementation. It will need to allow for the occasional dropped frame without a meltdown

			fprintf(stderr, "ERROR: Frame is null\n");
			getchar();
			break;
		}
		data_input_time_file << GetCounter() - data_input_timer << ", ";

		SysString^ grbl_status;
		int grbl_state = 0;

		data_input_timer = GetCounter();
		do
		{
			grbl_status = arduino_rx(arduino);
			if (grbl_status->Equals("ok\r"));
			{
				moves_in_queue--;
			}
		} while (grbl_status->Equals("ok\r"));

		if (grbl_status->Contains("Idle"))
		{
			grbl_state = GRBL_STATE_IDLE;
		}
		else if (grbl_status->Contains("Run"))
		{
			grbl_state = GRBL_STATE_RUN;
			ready_to_send_next_move_cmd = true;
		}
		else
		{
			Console::WriteLine(grbl_status);
			getchar();
			goto exit_main_loop;
		}
		data_input_time_file << GetCounter() - data_input_timer << ", ";

		data_input_timer = GetCounter();
		// Convert to monochrome
		cvtColor(capped_frame, processed_frame, CV_BGR2GRAY);
		// Blur to reduce noise
		blur(processed_frame, processed_frame, Size(10, 10));
		// Threshold to convert to binary image for easier contouring
		int binary_threshold = 48; // out of 255
		threshold(processed_frame, processed_frame, binary_threshold, 255, CV_THRESH_BINARY);

		// Detect edges (I don't think the thresholds are that important here since the image has already been binarized. However, they are mandatory for the function call)
		Mat processed_frame_edges;
		int lower_canny_threshold=10;
		int upper_canny_threshold = lower_canny_threshold*10;
		misc_timer = GetCounter();
		Canny(processed_frame, processed_frame_edges, lower_canny_threshold, upper_canny_threshold);
		misc_time_file << GetCounter() - misc_timer << "\n";

		// Detect contours
		contours.clear(); // Clear upon each new iteration of the loop
		findContours(processed_frame_edges, contours, hierarchy, CV_RETR_TREE, CV_CHAIN_APPROX_SIMPLE);

		// Find the largest contour by calculating all contour areas and picking the largest one
		// TODO This doesn't work when the contour is concave, need to fix.
		double largest_contour_area = 0;
		double contour_area = 0;
		vector<Point> object_external_contour;
		for (int i = 0; i < contours.size(); i++)
		{
			contour_area = contourArea(contours[i]);
			if (largest_contour_area < contour_area)
			{
				largest_contour_area = contour_area;
				object_external_contour = contours[i];
			}
		}

		// This will be the drawing displayed on the stream if it's enabled
		Mat displayed_drawing = capped_frame; // Initialize with just the camera image. Outline and centroid will be added later
											  // Change the frame this is assigned to to change what you want to look at

		Point2f origin = Point2f(100, 100); // Center of the frame. (0, 0) is usually the upper left corner. Define this because offset will be relative to "origin"
		int dx=0, last_dx;
		int dy=0, last_dy;
		if (!object_external_contour.empty()) // Only do the following if there's a contour to work with, otherwise just skip to the next frame
		{
			// Find and add bounding ellipse to the drawing
			RotatedRect bounding_ellipse = fitEllipse(Mat(object_external_contour));
			int bounding_ellipse_thickness = 2;
			ellipse(displayed_drawing, bounding_ellipse, color_green, bounding_ellipse_thickness, 8); // add to drawing

			// Find the moment (i.e. center of mass) of the contour and add it to the drawing. This will be the point that is actually followed.
			Moments object_moment = moments(object_external_contour);
			Point2f object_coord = Point2f(float(object_moment.m10) / float(object_moment.m00), float(object_moment.m01) / float(object_moment.m00)); // convert moment to useable coordinates
			int moment_center_radius = 2;
			circle(displayed_drawing, object_coord, moment_center_radius, color_green, CV_FILLED); // add to drawing


			dx = last_dx = object_coord.x - origin.x; // Offset of the object centroid in x
			dy = last_dy = object_coord.y - origin.y; // Offset of the object centroid in y

			is_following = true;
		}
		else
		{
			if (is_following)
			{
				// If the camera was already following and the object has fallen off the frame, then "leap" in the same direction
				dx = 2.0*last_dx;
				dy = 2.0*last_dy;

				is_following = false; //Just do this once because if it can't find the object anymore then I don't want the thing crashing
			}
		}
		data_input_time_file << GetCounter() - data_input_timer << "\n";

		SysString^ gcode_command_type = "0";
		
		if (!stream_only && !console_enabled) // Don't move stuff if I just want to look at the camera or send manual commands
		{
			// Motion stuff based on the object's coordinate, also for this test I'm doing right now it only has X axis enabled
			// TODO Have the move command continue without an available contour just in the direction it was previously traveling

			// This is my janky control system that will definitely need to be improved

			float p_gain_x; // Proportional gain for x-axis
			float p_gain_y; // Proportional gain for y-axis
			if (abs(dx) > 80)
				p_gain_x = 0.01;
			else if (abs(dx) > 50)
				p_gain_x = 0.009;
			else if (abs(dx) > 15)
				p_gain_x = 0.006;
			//else if (abs(dx) > 5)
			//	p_gain_x = 0.002;
			else
				p_gain_x = 0;

			if (abs(dy) > 80)
				p_gain_y = 0.01;
			else if (abs(dy) > 50)
				p_gain_y = 0.009;
			else if (abs(dy) > 15)
				p_gain_y = 0.006;
			//else if (abs(dy) > 5)
			//	p_gain_y = 0.002;
			else
				p_gain_y = 0;

			float x_command = -dx*p_gain_x;
			float y_command = dy*p_gain_y;

			if ((x_command != 0 || y_command != 0) && grbl_state == GRBL_STATE_IDLE && ready_to_send_next_move_cmd && GetCounter() - command_timer > 1000/10)
			{
				SysString^ gcode_command = "G" + gcode_command_type + " X" + Convert::ToString(x_command) + " Y" + Convert::ToString(y_command);
				command_timer = GetCounter();
				arduino_tx(arduino, gcode_command);
				moves_in_queue++;
				command_time_file << GetCounter() - command_timer << "\n";
				//Console::WriteLine(GetCounter() - command_timer); 
				//Console::WriteLine(moves_in_queue);
				ready_to_send_next_move_cmd = false;
			}
		}

		if (stream_enabled || stream_only)
		{
			imshow("Frame", displayed_drawing);
			char key_press = waitKey(1);
			if (key_press == 27) return 0;
		}

		if (console_enabled)
		{
			serial_message = Console::ReadLine();

			if (serial_message->Equals("exit")) break;
			arduino_tx(arduino, serial_message);

			try 
			{
				while (true)
				{
					serial_response = arduino_rx(arduino, 1000);
					Console::WriteLine(serial_response);
				}
				
			}
			catch (TimeoutException^ exception)
			{

			}
		}

		//loop_time_file << GetCounter() - loop_timer << "\n";
		//loop_timer = GetCounter();

		data_input_timer = GetCounter();
		arduino_tx(arduino, "?"); // Query Grbl status
		data_input_time_file << GetCounter() - data_input_timer << ", ";
	}

exit_main_loop:

	loop_time_file.close();
	command_time_file.close();
	data_input_time_file.close();
	Console::Clear();

	arduino->Close();

	return 0;
}
