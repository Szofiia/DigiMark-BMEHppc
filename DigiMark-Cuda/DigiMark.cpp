#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <iostream>

#include <chrono>

#include "Watermarking_CUDA.h"

using namespace cv;
using namespace std;

// Read image file from arguments
void processImageFromFile(Mat& I, char* fileName)
{
    // Read
    Mat image = imread(fileName, IMREAD_GRAYSCALE);

    if (image.empty()) // Check for invalid input
    {
        cout << "Could not open or find the image" << std::endl;
        exit(0);
    }

    // Padding for faster Fourier Transform
    Mat Ip;
    int M = getOptimalDFTSize(image.rows);
    int N = getOptimalDFTSize(image.cols);

    if (M != N) // Check for invalid input
    {
        cout << "Image width and height do not match" << std::endl;
        exit(0);
    }

    copyMakeBorder(image, Ip, 0, M - image.rows, 0, N - image.cols, BORDER_CONSTANT, Scalar::all(0));

    // Copy to I
    Ip.copyTo(I);
}

// Create nxn Watermark image 
void createWatermark(Mat& W, int n)
{
    RNG rng;
    W = Mat(n, n, CV_32F);
    cv::randu(W, 0, 1);
}

// Separate I into NxN blocks in V 
void createBlocks(vector<Mat>& V, Mat& I, int N)
{
    int M1 = I.rows;
    int M2 = I.cols;

    if (M1 != M2) // Check for invalid input
    {
        cout << "Image width and height do not match" << std::endl;
        exit(0);
    }

    for (int i = 0; i < M1 - N + 1; i += N)
    {
        for (int j = 0; j < M1 - N + 1; j += N)
        {
            Rect frame = Rect(i, j, N, N);
            Mat Vij = Mat(I, frame);
            V.push_back(Vij);
        }
    }
}

// Re-assemble blocks
void assembleBlocks(vector<Mat>& V, Mat& I, int n)
{
    for (int i = 0; i < V.size(); i++) {
        int row_num = (int)floor(i / n);
        int col_num = i % n;

        Mat dst_roi = I(Rect(row_num * V[i].cols, col_num * V[i].rows, V[i].cols, V[i].rows));

        V[i].copyTo(dst_roi);
    }
}

// Place a watermark according to a strategy
void placeWatermark(Mat& Vi, float W)
{
    // We exclude the DC component, that is
    // the highest magnitude, always in the top-left corner
    Mat temp;
    Vi.copyTo(temp);
    double min_val, max_val;
    Point min_loc, max_loc;

    minMaxLoc(Vi, &min_val, &max_val, &min_loc, &max_loc);
    temp.at<float>(max_loc) = 0;

    // Find highest magnitude coefficients
    minMaxLoc(temp, &min_val, &max_val, &min_loc, &max_loc);

    // Embed
    float alpha = 0.5;
    Vi.at<float>(max_loc) = Vi.at<float>(max_loc) + alpha * W;
}

int main(int argc, char** argv)
{
    std::cout << "SS Watermarking\n";

    if (argc != 2)
    {
        cout << "Error: Image not added as an argument." << endl;
        return -1;
    }

    // Load image
    Mat I;
    processImageFromFile(I, argv[1]);

    Mat converted;
    I.convertTo(converted, CV_32F, 1.0 / 255);
    
    // Create Watermark (Serial)
    int n = 8;
    Mat W;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    createWatermark(W, n);
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout << "Serial time difference = " << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "[�s]" << std::endl;

    // Create Watermark (Cuda Host API)
    int dim = n * n;
    std::vector<float> host_array(dim);

    begin = std::chrono::steady_clock::now();
    CalcRandWithHostAPI(host_array.data(), host_array.size());
    end = std::chrono::steady_clock::now();
    std::cout << "Cuda HostAPI time difference = " << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "[�s]" << std::endl;

    // Create Watermark (Cuda Device API)
    std::vector<float> dev_array(dim);

    begin = std::chrono::steady_clock::now();
    CalcRandWithDevAPI(dev_array.data(), dev_array.size());
    end = std::chrono::steady_clock::now();
    std::cout << "Cuda DevAPI time difference = " << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "[�s]" << std::endl;

    Mat host_api_mat = Mat(n, n, CV_32FC1, (float*)host_array.data());
    Mat dev_api_mat = Mat(n, n, CV_32FC1, (float*)dev_array.data());
    imshow("Host random", W);
    imshow("HostAPI random", host_api_mat);
    imshow("DevAPI random", dev_api_mat);

    // Write into file, png conversion
    Mat Iw;
    W.convertTo(Iw, CV_8U, 255.0);
    imwrite("watermark.png", Iw);

    // Divide image into N = width / n blocks, V = [v1, v2,...vN]
    int N = I.rows / n;
    vector<Mat> V;
    createBlocks(V, I, N);

    // DCT on each block
    vector<Mat> Vdct;
    vector<Mat> Vcdct;
    for (Mat Vi : V) {
        Vi.convertTo(Vi, CV_32F, 1.0 / 255);
        Mat Vti;
        dct(Vi, Vti);
        Vdct.push_back(Vti);
        Vcdct.push_back(Vti);
    }

    // Reassemble DCT
    Mat Idct = cv::Mat::zeros(512, 512, CV_32F);
    assembleBlocks(Vdct, Idct, n);

    Mat Iwdct;
    Idct.convertTo(Iwdct, CV_8U, 255.0);
    imwrite("dcts.png", Iwdct);

    // Place the Watermark in the most significant bits
    begin = std::chrono::steady_clock::now();
    for (int i = 0; i < Vdct.size(); i++) {
        int row_num = (int)floor(i / n);
        int col_num = i % n;
        float w = W.at<float>(row_num, col_num);
        placeWatermark(Vdct[i], w);
    }
    end = std::chrono::steady_clock::now();
    std::cout << "Host time difference = " << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "[�s]" << std::endl;

    // Inverse DCT
    vector<Mat> D;
    for (Mat Vti : Vdct) {
        Mat Di;
        idct(Vti, Di);
        D.push_back(Di);
    }

    Mat Id = cv::Mat::zeros(512, 512, CV_32F);
    assembleBlocks(D, Id, n);
    Id.convertTo(Id, CV_8U, 255.0);
    imwrite("lena_reassembled.png", Id);
    imshow("Host Watermarked", Id);

    vector<float> Data;

    begin = std::chrono::steady_clock::now();
    for (int i = 0; i < Vcdct.size(); i++) {
        int row_num = (int)floor(i / n);
        int col_num = i % n;
        float w = W.at<float>(row_num, col_num);

        Data.assign((float*)Vcdct[i].datastart, (float*)Vcdct[i].dataend);
        CalcWatermark(Data.data(), Data.size(), w, 1.2);
        memcpy(Vcdct[i].data, Data.data(), Data.size() * sizeof(float));
    }
    end = std::chrono::steady_clock::now();
    std::cout << "Cuda BLAS time difference = " << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "[�s]" << std::endl;

    // Inverse DCT
    vector<Mat> Dc;
    for (Mat Vti : Vcdct) {
        Mat Di;
        idct(Vti, Di);
        Dc.push_back(Di);
    }

    Mat Icd = cv::Mat::zeros(512, 512, CV_32F);
    assembleBlocks(Dc, Icd, n);
    Icd.convertTo(Icd, CV_8U, 255.0);
    imshow("Cuda Watermarked", Icd);

    waitKey(0);
    return 0;
}
