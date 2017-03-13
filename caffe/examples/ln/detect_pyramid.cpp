//
// Libor Novak
// 03/12/2017
//
// Detects objects by using a single scale detector on an image pyramid
//

#include <caffe/caffe.hpp>

// This code only works with OpenCV!
#ifdef USE_OPENCV

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <algorithm>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/program_options.hpp>
namespace po = boost::program_options;


/**
 * @brief Wraps the input layer into a vector of cv::Mat so we could assign data to it more easily
 * @param input_layer Pointer to the net input layer blob
 * @param input_channels Vector of cv::Mat, which will be assigned
 */
void wrapInputLayer (caffe::Blob<float>* input_layer, std::vector<cv::Mat> &out_input_channels)
{
    out_input_channels.clear();

    int height = input_layer->shape(2);
    int width  = input_layer->shape(3);

    float* input_data = input_layer->mutable_cpu_data();

    for (int i = 0; i < input_layer->shape(1); ++i)
    {
        cv::Mat channel(height, width, CV_32FC1, input_data);
        out_input_channels.push_back(channel);
        input_data += width * height;
    }
}


void runPyramidDetection (const std::string &path_prototxt, const std::string &path_caffemodel,
                          const std::string &path_image_list)
{
#ifdef CPU_ONLY
    caffe::Caffe::set_mode(caffe::Caffe::CPU);
#else
    caffe::Caffe::set_mode(caffe::Caffe::GPU);
#endif

    const std::vector<double> scales = {2.25, 1.5, 1.0, 0.66, 0.44};

    // Create network and load trained weights from caffemodel file
    auto net = std::make_shared<caffe::Net<float>>(path_prototxt, caffe::TEST);
    net->CopyTrainedLayersFrom(path_caffemodel);

    caffe::Blob<float>* input_layer  = net->input_blobs()[0];
    LOG(INFO) << "We have " << net->output_blobs().size() << " accumulators on the output";

    CHECK_EQ(net->num_inputs(), 1) << "Network should have exactly one input.";
    CHECK_EQ(input_layer->shape(1), 3) << "Input layer must have 3 channels.";
    for (const auto output_layer: net->output_blobs())
    {
        CHECK_EQ(output_layer->shape(1), 1) << "Output layer must have one channel";
    }

    // Prepare the input channels
    std::vector<cv::Mat> input_channels;
    wrapInputLayer(input_layer, input_channels);

    std::ifstream infile(path_image_list.c_str());
    CHECK(infile) << "Unable to open image list TXT file '" << path_image_list << "'!";
    std::string line; int i = 0;
    while (std::getline(infile, line))
    {
        LOG(INFO) << line;
        CHECK(boost::filesystem::exists(line)) << "Image '" << line << "' not found!";

        // Load the image
        cv::Mat image = cv::imread(line, CV_LOAD_IMAGE_COLOR);
        cv::Mat imagef; image.convertTo(imagef, CV_32FC3);

        // Convert to zero mean and unit variance
        imagef -= cv::Scalar(128.0f, 128.0f, 128.0f);
        imagef *= 1.0f/128.0f;

        // Build the image pyramid and run detection on each scale of the pyramid
        for (double s: scales)
        {
            cv::Mat imagef_scaled; cv::resize(imagef, imagef_scaled, cv::Size(), s, s);
            std::cout << "Current size: " << imagef_scaled.size() << " (" << s << ")" << std::endl;

            if (imagef_scaled.rows != input_layer->shape(2) || imagef_scaled.cols != input_layer->shape(3))
            {
                // Reshape the network
                input_layer->Reshape(1, input_layer->shape(1), imagef_scaled.rows, imagef_scaled.cols);
                net->Reshape();

                wrapInputLayer(input_layer, input_channels);
            }

            // Copy the image to the input layer of the network
            cv::split(imagef_scaled, input_channels);

            net->Forward();


            // Show the result
            caffe::Blob<float>* output_layer = net->output_blobs()[0];
            cv::Mat accumulator(output_layer->shape(2), output_layer->shape(3), CV_32FC1,
                                output_layer->mutable_cpu_data());

            double mx;
            cv::minMaxLoc(accumulator, 0, &mx);
            std::cout << mx << std::endl;

            cv::imshow("Accumulator " + net->blob_names()[net->output_blob_indices()[0]] + " (" + std::to_string(s) + ")", accumulator);

            // Draw detected boxes
            for (int i = 0; i < accumulator.rows; ++i)
            {
                for (int j = 0; j < accumulator.cols; ++j)
                {
                    float conf = accumulator.at<float>(i,j);
                    if (conf > 0.7)
                    {
                        cv::rectangle(image, cv::Rect(4*j/s-40/s, 4*i/s-40/s, 80/s, 80/s), cv::Scalar(0,0,255));
                    }
                }
            }
        }

        cv::imshow("Image", image);
        cv::waitKey(0);
    }
}



// -----------------------------------------------  MAIN  ------------------------------------------------ //

struct ProgramArguments
{
    std::string path_prototxt;
    std::string path_caffemodel;
    std::string path_image_list;
};


/**
 * @brief Parses arguments of the program
 */
void parseArguments (int argc, char** argv, ProgramArguments &pa)
{
    try {
        po::options_description desc("Arguments");
        desc.add_options()
            ("help", "Print help")
            ("prototxt", po::value<std::string>(&pa.path_prototxt)->required(),
             "Model file of the network (*.prototxt)")
            ("caffemodel", po::value<std::string>(&pa.path_caffemodel)->required(),
             "Weight file of the network (*.caffemodel)")
            ("image_list", po::value<std::string>(&pa.path_image_list)->required(),
             "Path to a TXT file with paths to the images to be tested")
        ;

        po::positional_options_description positional;
        positional.add("prototxt", 1);
        positional.add("caffemodel", 1);
        positional.add("image_list", 1);


        // Parse the input arguments
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).positional(positional).run(), vm);

        if (vm.count("help")) {
            std::cout << "Usage: ./detect_accumulator path/f.prototxt path/f.caffemodel path/image_list.txt\n";
            std::cout << desc;
            exit(EXIT_SUCCESS);
        }

        po::notify(vm);

        if (!boost::filesystem::exists(pa.path_prototxt))
        {
            std::cerr << "ERROR: File '" << pa.path_prototxt << "' does not exist!" << std::endl;
            exit(EXIT_FAILURE);
        }
        if (!boost::filesystem::exists(pa.path_caffemodel))
        {
            std::cerr << "ERROR: File '" << pa.path_caffemodel << "' does not exist!" << std::endl;
            exit(EXIT_FAILURE);
        }
        if (!boost::filesystem::exists(pa.path_image_list))
        {
            std::cerr << "ERROR: File '" << pa.path_image_list << "' does not exist!" << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    catch(std::exception& e)
    {
        std::cerr << e.what() << "\n";
        exit(EXIT_FAILURE);
    }
}


int main (int argc, char** argv)
{
    ::google::InitGoogleLogging(argv[0]);

    ProgramArguments pa;
    parseArguments(argc, argv, pa);


    runPyramidDetection(pa.path_prototxt, pa.path_caffemodel, pa.path_image_list);


    return EXIT_SUCCESS;
}


#else
int main(int argc, char** argv) {
    LOG(FATAL) << "This example requires OpenCV; compile with USE_OPENCV.";
}
#endif  // USE_OPENCV
