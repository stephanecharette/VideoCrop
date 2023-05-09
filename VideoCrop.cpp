#include <opencv2/opencv.hpp>
#include <exception>
#include <filesystem>
#include <cstdio>
#include <regex>


cv::Size smallest_screen;
cv::Rect mouse_click_rect;
cv::Rect frame_rect;


void get_display_dimensions()
{
	/* With a single monitor, the string will look like this:
	 *
	 *		Monitors: 1
	 *		 0: +*HDMI-0 1280/380x1024/310+0+0  HDMI-0
	 *
	 * With multiple monitiors, the output will look like this:
	 *
	 *		Monitors: 3
	 *		 0: +*HDMI-0 1920/531x1080/299+1280+0  HDMI-0
	 *		 1: +DVI-D-0 1920/531x1080/299+3200+0  DVI-D-0
	 *		 2: +DP-0 1280/380x1024/310+0+0  DP-0
	 */
	std::regex rx(
		" (\\d+)"	// 1st group:  width
		"\\/"
		"\\d+"
		"x"
		"(\\d+)"	// 2nd group:  height
		"\\/"
		"\\d");

	std::unique_ptr<FILE, decltype(&pclose)> file(popen("xrandr --listactivemonitors", "r"), pclose);

	std::vector<cv::Size> display_dimensions;
	while (std::ferror(file.get()) == 0 and std::feof(file.get()) == 0)
	{
		char buffer[200];
		auto ptr = fgets(buffer, sizeof(buffer), file.get());
		if (ptr != nullptr)
		{
			std::string str = ptr;
			std::smatch match;
			if (std::regex_search(str, match, rx))
			{
				const cv::Size s(
					std::stoi(match.str(1)),
					std::stoi(match.str(2)));
				display_dimensions.push_back(s);
				std::cout << "Display dimensions ..... " << s.width << " x " << s.height << std::endl;
			}
		}
	}

	if (display_dimensions.empty())
	{
		throw std::runtime_error("failed to determine the display dimensions");
	}

	smallest_screen = display_dimensions[0];
	for (const auto & size : display_dimensions)
	{
		if (size.area() < smallest_screen.area())
		{
			smallest_screen = size;
		}
	}

	std::cout << "Smallest display ....... " << smallest_screen.width << " x " << smallest_screen.height << std::endl;

	return;
}


void mouse_callback(int event, int x, int y, int flags, void * userdata)
{
	#if 0
	std::cout
		<< "mouse callback:"
		<< " x=" << x
		<< " y=" << y
		<< " flags=" << flags
		<< std::endl;
	#endif

	if (flags & cv::MouseEventTypes::EVENT_LBUTTONDOWN)
	{
		cv::Rect & r = mouse_click_rect; // alias since "r" is easier to type

		if (r.empty())
		{
			// this is our first mouse click
			r.x = x;
			r.y = y;
			r.width = 200;
			r.height = 200;
		}
		else
		{
			// we need to modify a corner -- determine which corner is nearest to this point

			const cv::Point p(x, y);

			const cv::Point corners[] =
			{
				cv::Point(r.x			, r.y				),
				cv::Point(r.x + r.width	, r.y				),
				cv::Point(r.x + r.width	, r.y + r.height	),
				cv::Point(r.x			, r.y + r.height	)
			};

			const double distances[] =
			{
				cv::norm(corners[0] - p),
				cv::norm(corners[1] - p),
				cv::norm(corners[2] - p),
				cv::norm(corners[3] - p)
			};

			// find the nearest corner
			size_t nearest_corner_idx = 0;
			for (size_t idx = 1; idx < 4; idx ++)
			{
				if (distances[idx] < distances[nearest_corner_idx])
				{
					nearest_corner_idx = idx;
				}
			}

			if (nearest_corner_idx == 0)
			{
				// top-left
				r.width		+= (corners[0].x - p.x);
				r.height	+= (corners[0].y - p.y);
				r.x = p.x;
				r.y = p.y;
			}
			else if (nearest_corner_idx == 1)
			{
				// top-right
				r.width		-= (corners[1].x - p.x);
				r.height	+= (corners[1].y - p.y);
				r.y = p.y;
			}
			else if (nearest_corner_idx == 2)
			{
				// bottom-right
				r.width		-= (corners[2].x - p.x);
				r.height	-= (corners[2].y - p.y);
			}
			else if (nearest_corner_idx == 3)
			{
				r.width		+= (corners[3].x - p.x);
				r.height	-= (corners[3].y - p.y);
				r.x = p.x;
			}
		}

		// check to make sure the rectangle is inside the frame

		if (r.x < frame_rect.x)
		{
			r.width -= (frame_rect.x - r.x);
			r.x = frame_rect.x;
		}
		if (r.y < frame_rect.y)
		{
			r.height -= (frame_rect.y - r.y);
			r.y = frame_rect.y;
		}
		if (r.x + r.width > frame_rect.x + frame_rect.width)
		{
			r.width = (frame_rect.x + frame_rect.width) - r.x;
		}
		if (r.y + r.height > frame_rect.y + frame_rect.height)
		{
			r.height = (frame_rect.y + frame_rect.height) - r.y;
		}
	}

	return;
}


void determine_rect(const std::filesystem::path & input_video_filename, cv::Rect & cropping_rect)
{
	mouse_click_rect = cv::Rect();

	cv::VideoCapture cap(input_video_filename);

	const int border_size			= 25; // in pixels
	const double video_fps			= cap.get(cv::VideoCaptureProperties::CAP_PROP_FPS			);
	const double video_width		= cap.get(cv::VideoCaptureProperties::CAP_PROP_FRAME_WIDTH	);
	const double video_height		= cap.get(cv::VideoCaptureProperties::CAP_PROP_FRAME_HEIGHT	);
	const double total_frames		= cap.get(cv::VideoCaptureProperties::CAP_PROP_FRAME_COUNT	);
	const double length_in_seconds	= total_frames / video_fps;
	const std::string length_string	=
		std::to_string(static_cast<int>(length_in_seconds) / 60) + "m " +
		std::to_string(static_cast<int>(length_in_seconds) % 60) + "s";

	// since we know the size of the smallest screen, and the size of the video, we can figure out a scaling factor to use
	double scale_factor = 1.0;
	cv::Size scaled_dimensions;
	cv::Size canvas_dimensions;
	while (true)
	{
		scaled_dimensions.width		= std::round(scale_factor * video_width);
		scaled_dimensions.height	= std::round(scale_factor * video_height);
		canvas_dimensions.width		= border_size * 2 + scaled_dimensions.width;
		canvas_dimensions.height	= border_size * 2 + scaled_dimensions.height;

		if (canvas_dimensions.width		>= 0.98 * smallest_screen.width or
			canvas_dimensions.height	>= 0.98 * smallest_screen.height)
		{
			scale_factor -= 0.02;
			continue;
		}

		break;
	}
	frame_rect = cv::Rect(border_size, border_size, scaled_dimensions.width, scaled_dimensions.height);

	std::cout
		<< ""																										<< std::endl
		<< "Input video filename ... " << input_video_filename.string()												<< std::endl
		<< "Frame rate ............. " << video_fps << " FPS"														<< std::endl
		<< "Dimensions ............. " << static_cast<int>(video_width) << " x " << static_cast<int>(video_height)	<< std::endl
		<< "Number of frames ....... " << total_frames																<< std::endl
		<< "Length of video ........ " << length_string																<< std::endl
		<< "Scale factor ........... " << scale_factor																<< std::endl
		<< "Scaled video size ...... " << scaled_dimensions.width << " x " << scaled_dimensions.height				<< std::endl;

	std::stringstream ss;
	ss	<< input_video_filename.filename().string()
		<< " "
		<< static_cast<int>(video_width)
		<< "x"
		<< static_cast<int>(video_height);
	if (scale_factor != 1.0)
	{
		ss	<< " @ "
			<< std::round(100.0 * scale_factor) << "%"
			<< " = "
			<< scaled_dimensions.width
			<< "x"
			<< scaled_dimensions.height;
	}
	cv::namedWindow("VideoCrop",
			cv::WindowFlags::WINDOW_AUTOSIZE +		// window cannot be resized
			cv::WindowFlags::WINDOW_KEEPRATIO +		// image ratio is maintained
			cv::WindowFlags::WINDOW_GUI_NORMAL);	// old-style window without status bar and toolbar

	cv::resizeWindow("VideoCrop", canvas_dimensions);
	cv::setWindowTitle("VideoCrop", ss.str());

	cv::setMouseCallback("VideoCrop", mouse_callback);

	cv::Mat frame;
	size_t next_frame_idx = 0;
	bool is_paused = false;

	while (cap.isOpened())
	{
		if (not is_paused)
		{
			cap >> frame;
			if (frame.empty())
			{
				std::cout << "failed to get frame #" << next_frame_idx << " from " << input_video_filename << std::endl;
				if (next_frame_idx == 0)
				{
					throw std::runtime_error("failed to read video " + input_video_filename.string());
				}
				next_frame_idx = 0;
				cap.set(cv::VideoCaptureProperties::CAP_PROP_POS_FRAMES, 0);
				continue;
			}
			next_frame_idx ++;
		}

		cv::Mat scaled_frame;
		if (scale_factor == 1.0)
		{
			scaled_frame = frame;
		}
		else
		{
			cv::resize(frame, scaled_frame, scaled_dimensions);
		}
		cv::Mat canvas(canvas_dimensions, CV_8UC3, {255, 255, 255});
		scaled_frame.copyTo(canvas(frame_rect));

		if (not mouse_click_rect.empty())
		{
			cv::rectangle(canvas, mouse_click_rect, {255, 0, 0}, 1, cv::LINE_AA);

			const std::string text =
				std::to_string(static_cast<int>(std::round(mouse_click_rect.width / scale_factor))) +
				" x " +
				std::to_string(static_cast<int>(std::round(mouse_click_rect.height / scale_factor)));
			cv::putText(canvas, text, mouse_click_rect.tl() + cv::Point(border_size, 2 * border_size), cv::FONT_HERSHEY_PLAIN, 2.0, {255, 0, 0}, 2, cv::LINE_AA);
		}

		cv::imshow("VideoCrop", canvas);
		const auto key = cv::waitKey(2);
		if (key == 27) // ESC
		{
			std::cout << "CANCEL!" << std::endl;
			break;
		}
		if (key == 32) // spacebar
		{
			is_paused = not is_paused;
		}
		if (key == 13) // enter
		{
			if (not mouse_click_rect.empty())
			{
				cropping_rect = mouse_click_rect - cv::Point(border_size, border_size);
				cropping_rect.x			= std::round(cropping_rect.x		/ scale_factor);
				cropping_rect.y			= std::round(cropping_rect.y		/ scale_factor);
				cropping_rect.width		= std::round(cropping_rect.width	/ scale_factor);
				cropping_rect.height	= std::round(cropping_rect.height	/ scale_factor);

				std::cout << "Crop rect .............. x=" << cropping_rect.x << " y=" << cropping_rect.y << " w=" << cropping_rect.width << " h=" << cropping_rect.height << std::endl;

				break;
			}
		}
		if (key > 0)
		{
			std::cout << "KEY=" << key << std::endl;
		}
	}

	cv::destroyAllWindows();

	return;
}


void crop_video(const std::filesystem::path & input_video_filename, const cv::Rect & cropping_rect)
{
	cv::VideoCapture cap(input_video_filename);

	const double video_fps		= cap.get(cv::VideoCaptureProperties::CAP_PROP_FPS			);
	const double total_frames	= cap.get(cv::VideoCaptureProperties::CAP_PROP_FRAME_COUNT	);
	const cv::Size final_size	= cropping_rect.size();

	const std::string output_video_filename =
			input_video_filename.stem().string() +
			"_crop_" +
			std::to_string(cropping_rect.width) +
			"x" +
			std::to_string(cropping_rect.height) +
			".m4v";

	std::cout
		<< ""																										<< std::endl
		<< "Input video filename ... " << input_video_filename.string()												<< std::endl
		<< "Output video filename .. " << output_video_filename														<< std::endl
		<< "Frame rate ............. " << video_fps << " FPS"														<< std::endl
		<< "Output dimensions ...... " << final_size.width << " x " << final_size.height							<< std::endl
		<< "Number of frames ....... " << total_frames																<< std::endl;

	cv::VideoWriter output(output_video_filename, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), video_fps, final_size);
	if (not output.isOpened())
	{
		throw std::runtime_error("failed to open output file " + output_video_filename);
	}

	const size_t key_frame = std::round(4.0 * video_fps);
	size_t current_frame = 0;

	while (cap.isOpened())
	{
		cv::Mat frame;
		cap >> frame;
		if (frame.empty())
		{
			break;
		}

		current_frame ++;
		if (current_frame % key_frame == 0)
		{
			std::cout
				<< "\r"
				<< "Cropping video ......... " << std::setprecision(1) << (current_frame * 100.0 / total_frames) << "% " << std::flush;
		}

		cv::Mat cropped = frame(cropping_rect);
		output.write(cropped);
	}

	std::cout << std::endl;

	return;
}


int main(int argc, char * argv[])
{
	int rc = 1;

	try
	{
		if (argc < 2)
		{
			std::cout
				<< "Usage:"												<< std::endl
				<< ""													<< std::endl
				<< "\t" << argv[0] << " <filename> [...]"				<< std::endl
				<< ""													<< std::endl
				<< "where <filename> is the video file to be cropped"	<< std::endl;

			throw std::invalid_argument("must specify at least one video filename");
		}

		// before we start, make sure all filenames are accessible
		for (int idx = 1; idx < argc; idx ++)
		{
			std::filesystem::path input_video_filename = argv[idx];
			if (not std::filesystem::exists(input_video_filename))
			{
				throw std::invalid_argument("video file \"" + input_video_filename.string() + "\" does not exist");
			}
		}

		get_display_dimensions();

		for (int idx = 1; idx < argc; idx ++)
		{
			std::filesystem::path input_video_filename = argv[idx];

			cv::Rect cropping_rect;
			std::cout << std::fixed << std::setprecision(-1);
			determine_rect(input_video_filename, cropping_rect);
			if (not cropping_rect.empty())
			{
				crop_video(input_video_filename, cropping_rect);
			}
		}

		rc = 0;
	}
	catch (const std::exception & e)
	{
		std::cout
			<< std::endl
			<< "------" << std::endl
			<< "ERROR: " << e.what() << std::endl;

		rc = 1;
	}
	catch (...)
	{
		std::cout
			<< std::endl
			<< "------" << std::endl
			<< "ERROR: unknown exception caught" << std::endl;

		rc = 2;
	}

	return rc;
}
