#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"

Estimator estimator;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<sensor_msgs::PointCloudConstPtr> relo_buf;
int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_estimator;

double latest_time;
Eigen::Vector3d tmp_P;
Eigen::Quaterniond tmp_Q;
Eigen::Vector3d tmp_V;
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;
bool init_feature = 0;
bool init_imu = 1;
double last_imu_t = 0;

/**
 * @brief: 从IMU测量值imu_msg和上一个PVQ递推得到下一个tmp_Q，tmp_P，tmp_V，中值积分
 * @param[in] imu_msg
 */
void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();

    // init_imu=1表示第一个IMU数据
    if (init_imu)
    {
        latest_time = t;
        init_imu = 0;
        return;
    }
    double dt = t - latest_time;
    latest_time = t;

    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    // 中值陀螺仪的结果
    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;

    // dq =  【1 w*dt/2】
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    // 上一时刻世界坐标系下加速度值
    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba) - estimator.g;
    // 当前时刻世界坐标系下加速度的值
    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba) - estimator.g;
    // 中值加速度计的值
    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);

    // P1 = P0 + v_0 * t + 0.5*a*t*t
    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    // 赋值给上一帧
    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = estimator.Ps[WINDOW_SIZE];
    tmp_Q = estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());
}

std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>>
getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    while (true)
    {
        if (imu_buf.empty() || feature_buf.empty())
            return measurements;

        //对齐标准：IMU最后一个数据的时间要大于第一个图像特征数据的时间
        // imu   *******
        // image          *   *  *   *   *
        // 这就是imu还没来
        if (!(imu_buf.back()->header.stamp.toSec() > feature_buf.front()->header.stamp.toSec() + estimator.td))
        {
            // ROS_WARN("wait for imu, only should happen at the beginning");
            sum_of_wait++;
            return measurements;
        }

        //对齐标准：IMU第一个数据的时间要小于第一个图像特征数据的时间
        // imu        ********
        // image    *   *   *   *   *   *
        // 这种只能扔掉一些image帧
        if (!(imu_buf.front()->header.stamp.toSec() < feature_buf.front()->header.stamp.toSec() + estimator.td))
        {
            ROS_WARN("throw img, only should happen at the beginning");
            feature_buf.pop();
            continue;
        }

        // 此时就保证了图像前一定有imu数据
        sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front();
        feature_buf.pop();

        // 一般第一帧不会严格对齐，但是后面就都会对齐，当然第一帧也不会用到
        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        while (imu_buf.front()->header.stamp.toSec() < img_msg->header.stamp.toSec() + estimator.td)
        {
            // emplace_back相比push_back能更好地避免内存的拷贝
            IMUs.emplace_back(imu_buf.front());
            imu_buf.pop();
        }

        // 保留图像时间戳后一个imu数据，但不会从buffer中扔掉
        // imu    *   *
        // image    *
        // 这里把下一个imu_msg也放进去了,但没有pop，因此当前图像帧和下一图像帧会共用这个imu_msg
        IMUs.emplace_back(imu_buf.front());

        if (IMUs.empty())
            ROS_WARN("no imu between two image");
        measurements.emplace_back(IMUs, img_msg);
    }
    return measurements;
}

/**
 * @brief imu回调函数，将imu_msg保存到imu_buf，IMU状态递推并发布[P,Q,V,header]
 * @param {ImuConstPtr} &imu_msg
 * @return {*}
 */
void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{

    // 判断时间间隔是否为正
    if (imu_msg->header.stamp.toSec() <= last_imu_t)
    {
        ROS_WARN("imu message in disorder! return !");
        return;
    }

    last_imu_t = imu_msg->header.stamp.toSec();

    m_buf.lock();
    imu_buf.push(imu_msg);
    m_buf.unlock();

    //唤醒作用于process线程中的获取观测值数据的函数
    con.notify_one();

    // 重复赋值？
    last_imu_t = imu_msg->header.stamp.toSec();

    {
        std::lock_guard<std::mutex> lg(m_state);

        //递推得到IMU的PQV
        predict(imu_msg);

        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";

        // 初始化完成后，发布IMU预测的位姿（高频）
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);
    }
}

/**
 * @description: feature回调函数，将feature_msg放入feature_buf
 * @param {PointCloudConstPtr} &feature_msg
 * @return {*}
 */
void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    if (!init_feature)
    {
        // skip the first detected feature, which doesn't contain optical flow speed
        init_feature = 1;
        return;
    }

    m_buf.lock();
    feature_buf.push(feature_msg);
    m_buf.unlock();

    // 特征的buf有数据了,通知一下进行处理
    con.notify_one();
}

/**
 *
 * @brief restart回调函数，收到restart时清空feature_buf和imu_buf，估计器重置，时间重置
 * @param {in} restart_msg
 * @return {*}
 */
void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data == true)
    {
        ROS_WARN("restart the estimator!");
        m_buf.lock();
        while (!feature_buf.empty())
            feature_buf.pop();
        while (!imu_buf.empty())
            imu_buf.pop();
        m_buf.unlock();
        m_estimator.lock();
        estimator.clearState();
        estimator.setParameter();
        m_estimator.unlock();
        current_time = -1;
        last_imu_t = 0;
    }
    return;
}

/**
 * @brief relocalization回调函数，将points_msg放入relo_buf
 * @param {PointCloudConstPtr} &points_msg
 * @return {*}
 */
void relocalization_callback(const sensor_msgs::PointCloudConstPtr &points_msg)
{

    // ROS_INFO("relocalization callback! \n");
    m_buf.lock();
    relo_buf.push(points_msg);
    m_buf.unlock();
}

/**
 * @brief   VIO的主线程:
 *              等待并获取measurements：(IMUs, img_msg)s，计算dt
 *              estimator.processIMU()进行IMU预积分
 *              estimator.setReloFrame()设置重定位帧
 *              estimator.processImage()处理图像帧：初始化，紧耦合的非线性优化
 * @return      void
 */
void process()
{
    while (true)
    {

        //
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;
        std::unique_lock<std::mutex> lk(m_buf);

        // feature_callback 和 imu_callback 通知唤醒
        // 如果存在有效的measurements，则上锁
        // 否则 解锁，并将程序阻塞在这儿
        con.wait(lk, [&]
                 { return (measurements = getMeasurements()).size() != 0; });

        // 数据buffer的锁解锁，回调可以继续塞数据了
        lk.unlock();
        // 进行后端求解，不能和复位重启冲突
        m_estimator.lock();

        // 给予范围的for循环，这里就是遍历每组image imu组合
        for (auto &measurement : measurements)
        {
            //对应这段的img data
            auto img_msg = measurement.second;
            // 加速度和角速度
            double dx = 0, dy = 0, dz = 0, rx = 0, ry = 0, rz = 0;

            // 遍历IMU
            for (auto &imu_msg : measurement.first)
            {
                double t = imu_msg->header.stamp.toSec();
                double img_t = img_msg->header.stamp.toSec() + estimator.td;

                // 发送IMU数据进行预积分
                if (t <= img_t)
                {
                    if (current_time < 0)
                        current_time = t;
                    // 计算时间差
                    double dt = t - current_time;
                    ROS_ASSERT(dt >= 0);
                    current_time = t;
                    dx = imu_msg->linear_acceleration.x;
                    dy = imu_msg->linear_acceleration.y;
                    dz = imu_msg->linear_acceleration.z;
                    rx = imu_msg->angular_velocity.x;
                    ry = imu_msg->angular_velocity.y;
                    rz = imu_msg->angular_velocity.z;
                    // 时间差和imu数据送进去
                    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    // printf("imu: dt:%f a: %f %f %f w: %f %f %f\n",dt, dx, dy, dz, rx, ry, rz);
                }
                else // 这就是针对最后一个imu数据，需要做一个简单的线性插值
                {
                    double dt_1 = img_t - current_time;
                    double dt_2 = t - img_t;
                    current_time = img_t;
                    ROS_ASSERT(dt_1 >= 0);
                    ROS_ASSERT(dt_2 >= 0);
                    ROS_ASSERT(dt_1 + dt_2 > 0);
                    double w1 = dt_2 / (dt_1 + dt_2);
                    double w2 = dt_1 / (dt_1 + dt_2);
                    dx = w1 * dx + w2 * imu_msg->linear_acceleration.x;
                    dy = w1 * dy + w2 * imu_msg->linear_acceleration.y;
                    dz = w1 * dz + w2 * imu_msg->linear_acceleration.z;
                    rx = w1 * rx + w2 * imu_msg->angular_velocity.x;
                    ry = w1 * ry + w2 * imu_msg->angular_velocity.y;
                    rz = w1 * rz + w2 * imu_msg->angular_velocity.z;
                    estimator.processIMU(dt_1, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    // printf("dimu: dt:%f a: %f %f %f w: %f %f %f\n",dt_1, dx, dy, dz, rx, ry, rz);
                }
            }

            // set relocalization frame
            // 回环相关部分
            sensor_msgs::PointCloudConstPtr relo_msg = NULL;
            while (!relo_buf.empty()) // 取出最新的回环帧
            {
                relo_msg = relo_buf.front();
                relo_buf.pop();
            }

            // 有效回环信息
            if (relo_msg != NULL)
            {
                vector<Vector3d> match_points;

                // 回环的当前帧时间戳
                double frame_stamp = relo_msg->header.stamp.toSec();
                for (unsigned int i = 0; i < relo_msg->points.size(); i++)
                {
                    Vector3d u_v_id;
                    u_v_id.x() = relo_msg->points[i].x;
                    u_v_id.y() = relo_msg->points[i].y;
                    u_v_id.z() = relo_msg->points[i].z;
                    match_points.push_back(u_v_id);
                }
                // 回环帧的位姿
                Vector3d relo_t(relo_msg->channels[0].values[0], relo_msg->channels[0].values[1], relo_msg->channels[0].values[2]);
                Quaterniond relo_q(relo_msg->channels[0].values[3], relo_msg->channels[0].values[4], relo_msg->channels[0].values[5], relo_msg->channels[0].values[6]);
                Matrix3d relo_r = relo_q.toRotationMatrix();
                int frame_index;
                frame_index = relo_msg->channels[0].values[7];
                estimator.setReloFrame(frame_stamp, frame_index, match_points, relo_t, relo_r);
            }

            // 开始处理图像数据
            ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());

            TicToc t_s;
            //建立每个特征点的(camera_id,[x,y,z,u,v,vx,vy])s的map，索引为feature_id
            map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> image;
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                double p_u = img_msg->channels[1].values[i];
                double p_v = img_msg->channels[2].values[i];
                double velocity_x = img_msg->channels[3].values[i];
                double velocity_y = img_msg->channels[4].values[i];
                ROS_ASSERT(z == 1);
                Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
                xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
                image[feature_id].emplace_back(camera_id, xyz_uv_velocity);
            }
            // 处理图像特征
            estimator.processImage(image, img_msg->header);

            // 输出统计信息
            double whole_t = t_s.toc();
            printStatistics(estimator, whole_t);

            //给RVIZ发送topic
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";

            pubOdometry(estimator, header);   //"odometry" 里程计信息PQV
            pubKeyPoses(estimator, header);   //"key_poses" 关键点三维坐标
            pubCameraPose(estimator, header); //"camera_pose" 相机位姿
            pubPointCloud(estimator, header); //"history_cloud" 点云信息
            pubTF(estimator, header);         //"extrinsic" 相机到IMU的外参
            pubKeyframe(estimator);           //"keyframe_point"、"keyframe_pose" 关键帧位姿和点云

            if (relo_msg != NULL)
                pubRelocalization(estimator); //"relo_relative_pose" 重定位位姿
            // ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }
        m_estimator.unlock();
        m_buf.lock();
        m_state.lock();

        
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}

int main(int argc, char **argv)
{
    // ROS初始化,设置句柄
    // 私有命名空间
    ros::init(argc, argv, "vins_estimator");
    ros::NodeHandle n("~");

    // 设置消息等级
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);

    // 从config文件中读取参数
    readParameters(n);

    // 根据读取的参数设置估计器
    estimator.setParameter();

#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif
    ROS_WARN("waiting for image and imu...");

    // 注册一些publisher 用于显示
    registerPub(n);

    //订阅IMU、feature、restart、match_points的topic,执行各自回调函数
    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_restart = n.subscribe("/feature_tracker/restart", 2000, restart_callback);
    ros::Subscriber sub_relo_points = n.subscribe("/pose_graph/match_points", 2000, relocalization_callback);

    // VIO主线程
    std::thread measurement_process{process};
    ros::spin();

    return 0;
}
