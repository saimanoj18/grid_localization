/*--------------------------------------------------------------------------
 *
 --------------------------------------------------------------------------*/

#include "pointcloud_receive.h"

/*---------------------------------------------------------------------------
                    Initialization part
----------------------------------------------------------------------------*/
pointcloud_receive::pointcloud_receive()
{
    //NH
    ros::NodeHandle nh;
    ros::NodeHandle pnh("~");

    //subscribe
    sub_velodyne_front = new message_filters::Subscriber<sensor_msgs::PointCloud2> (nh, "/front/velodyne_points", 2);
    sub_velodyne_rear = new message_filters::Subscriber<sensor_msgs::PointCloud2> (nh, "/rear/velodyne_points", 2);
    sync = new Synchronizer<MySyncPolicy> (MySyncPolicy(10), *sub_velodyne_front, *sub_velodyne_rear);
    sync->registerCallback(boost::bind(&pointcloud_receive::pointcloud_callback, this, _1, _2));

    //publishe
    pose_pub = nh.advertise<std_msgs::Float64MultiArray>("pose_vlp16",2);

    /*-------------------ros parameter initialization--------------------------*/
    pnh.param<int>("excute_mode", excute_mode, 2);
    pnh.param<string>("pointcloud_file", pointcloud_file, "/home/guolindong/catkin_ws/src/grid_localization/pointcloud/cyberfly_g200_oct_cloud.ply");
    pnh.param<string>("gridmap_path", gridmap_path, "/home/guolindong/catkin_ws/src/grid_localization/map/cyberfly_g200_oct");
    pnh.param<string>("log_file_path", log_file_path, "/home/guolindong/catkin_ws/src/grid_localization/save_log");
    pnh.param<string>("config_file_path", config_file_path, "/home/guolindong/catkin_ws/src/grid_localization/config");
    //get param
    pnh.getParam("excute_mode", excute_mode);
    pnh.getParam("pointcloud_file", pointcloud_file);
    pnh.getParam("gridmap_path", gridmap_path);
    pnh.getParam("log_file_path", log_file_path);
    pnh.getParam("config_file_path", config_file_path);
    //print param

    ROS_INFO("pointcloud_file: %s",pointcloud_file.c_str());
    ROS_INFO("gridmap_path: %s",gridmap_path.c_str());
    ROS_INFO("log_file_path: %s",log_file_path.c_str());
    ROS_INFO("config_file_path: %s", config_file_path.c_str());

    /*--------------------MRPT parameter initialization------------------------*/
    //grid_localization configuration initialization
    grid_localization_init(); //init part show be called after getting excute_mode

}


/*---------------------------------------------------------------------------
                    LiDAR callback, Main process
----------------------------------------------------------------------------*/
void pointcloud_receive::pointcloud_callback(
        const sensor_msgs::PointCloud2::ConstPtr& front,
        const sensor_msgs::PointCloud2::ConstPtr& rear)
//void pointcloud_receive::pointcloud_callback(const sensor_msgs::PointCloud2 & msg)
{
    mainLoop.Tic();//start the stop watch

    /*----------------------Get LiDAR data and form points map--------------------*/
    try{
        CColouredPointsMap curPointsMapColoredTemp,curPointsMapColoredFront,curPointsMapColoredRear;
        mrpt_bridge::copy2colouredPointsMap(front,curPointsMapColoredFront); //to CColoredPointsMap
        mrpt_bridge::copy2colouredPointsMap(rear,curPointsMapColoredRear);

        curPointsMapColoredFront.changeCoordinatesReference(
            CPose3D(pose_front_VLP16_x,
                    pose_front_VLP16_y,
                    pose_front_VLP16_z,
                    pose_front_VLP16_yaw,
                    pose_front_VLP16_pitch,
                    pose_front_VLP16_roll));

        curPointsMapColoredRear.changeCoordinatesReference(
            CPose3D(pose_rear_VLP16_x,
                    pose_rear_VLP16_y,
                    pose_rear_VLP16_z,
                    pose_rear_VLP16_yaw,
                    pose_rear_VLP16_pitch,
                    pose_rear_VLP16_roll));

        curPointsMapColoredTemp.insertionOptions.minDistBetweenLaserPoints = minDisBetweenLaserPoints;
        curPointsMapColoredTemp.insertAnotherMap(&curPointsMapColoredFront,CPose3D(0,0,0,0,0,0));
        curPointsMapColoredTemp.insertAnotherMap(&curPointsMapColoredRear,CPose3D(0,0,0,0,0,0));

        float max_x, max_y, max_z, min_x, min_y, min_z;
        curPointsMapColoredTemp.boundingBox(min_x, max_x, min_y, max_y, min_z, max_z);
        curPointsMapColoredTemp.extractPoints(
            TPoint3D(min_x, min_y, pointsMap_heightMin), 
            TPoint3D(max_x, max_y, pointsMap_heightMax), 
            &curPointsMapColored,
            1,0.1,0.1);

    }
    catch (ros::Exception &e){
        ROS_ERROR("pointcloud_callback exception: %s", e.what());
        return;
    }

    /*---------------------------Load/generate/save grid map file------------------------*/
    //if don't use gps for mapping and localization
    if(isPoseEstGood)
    {
        if (DR.firstGpsUpdate)
        {
            DR.calculate_pose_inc();    //for ordinary usage, like localization
            DR.get_poseGps_ekf();   //use poseGps_ekf as GT?
            //DR.get_pose_ekf();
            poseIncr2D = CPose2D(DR.robot_pose_inc.x(),DR.robot_pose_inc.y(),DR.robot_pose_inc.phi());
            poseDR2D += poseIncr2D;

            //if need relocate using ICP according to the current GPS(or if ICP failed) 
            CPoint2D pointEst2D = CPoint2D(poseEst2D.x(),poseEst2D.y());
            poseEkf2D = CPose2D(DR.robot_poseGps_ekf.x(),DR.robot_poseGps_ekf.y(),DR.robot_poseGps_ekf.phi());
            if (isNan(poseEkf2D.x()) || isNan(poseEkf2D.y() ))
            {
                X0_init(0,0) = poseEkf2D_last.x();
                X0_init(1,0) = poseEkf2D_last.y();
                X0_init(2,0) = poseEkf2D_last.phi();
                DR.GPSINS_EKF.init(3,X0_init,P0_init);//Nan
            }
            if (isNan(poseEst2D.x()) || isNan(poseEst2D.y() ))
            {
                X0_init(0,0) = poseEkf2D_last.x();
                X0_init(1,0) = poseEkf2D_last.y();
                X0_init(2,0) = poseEkf2D_last.phi();
                DR.glResult_EKF.init(3,X0_init,P0_init);//NaN
            } 

            //if( poseEstDist2poseEKF > poseErrorMax)
            if(info.goodness < icp_goodness_threshold)
            {
                isPoseEstGood = false;
                DR.notDoingIcpYet = true;
                initialGuessStableCounter = 0;
            }

            //if (LOAD_POINTCLOUD)
            {
                poseEst2D_last = poseEst2D;
                poseEkf2D_last = poseEkf2D;
            }
            if(SAVE_POINTCLOUD) poseEst2D_last = poseEkf2D;
        }
    }
    else{
        DR.get_poseGps_ekf();//initialGuessStableCounter if use RTK-GPS to generate map
        poseEkf2D = CPose2D(DR.robot_poseGps_ekf.x(), DR.robot_poseGps_ekf.y(), DR.robot_poseGps_ekf.phi());
        if (isNan(poseEkf2D.x()) || isNan(poseEkf2D.y()) )
        {
            X0_init(0,0) = poseEkf2D_last.x();
            X0_init(1,0) = poseEkf2D_last.y();
            X0_init(2,0) = poseEkf2D_last.phi();
            DR.GPSINS_EKF.init(3,X0_init,P0_init);//NaN
        }
        if (isNan(poseEst2D.x()) || isNan(poseEst2D.y() ))
        {
            X0_init(0,0) = poseEkf2D_last.x();
            X0_init(1,0) = poseEkf2D_last.y();
            X0_init(2,0) = poseEkf2D_last.phi();
            DR.glResult_EKF.init(3,X0_init,P0_init);//NaN
        }
        poseEst2D = poseEkf2D;
        CPoint2D pointEst2D = CPoint2D(poseEst2D.x(),poseEst2D.y());

        //for matching using history pointclouds
        poseIncr2D = CPose2D(DR.robot_pose_inc.x(), DR.robot_pose_inc.y(), DR.robot_pose_inc.phi());
        poseDR2D += poseIncr2D;

        initialGuessStableCounter++;//follow gps for a while
        if(initialGuessStableCounter > gpsInitialStableCounter)
        {
            icpStarted = true;
            isPoseEstGood = true;
        }
        else icpStarted = false;//waiting for a stable gps used for ICP initialguess

        poseEst2D_last = poseEst2D;
        poseEkf2D_last = poseEkf2D;  
    }// end of else yaw_angle_veolcity_sum

    /*----------------------Insert LiDAR data into global points map----------------------*/
    if(!USE_GLOBALPOINTCLOUDFORMATCHING)
    {
        if(SAVE_POINTCLOUD)
        {
            float tt = tictac.Tac();
            if((tt>timeForGps) && (step % SAVE_POINTCLOUDSTEP == 0) && (DR.velocity!=0))
            {
                globalPointsMap.insertAnotherMap(
                        &curPointsMapColored,
                        CPose3D(poseEkf2D.x(),poseEkf2D.y(),0,poseEkf2D.phi(),0,0)//use GPS for mapping
                        //CPose3D(poseEst2D.x(),poseEst2D.y(),0,poseEst2D.phi(),0,0)//use localization for mapping
                );//insert only when velocity is not zero
            }
        }
        step++;
    }

    /*-----------------------Load/generate/save grid map file----------------------------*/
    if(isGlobalGridMapCenterChange(poseEst2D.x(), poseEst2D.y(), curGridMapCenter.x(), curGridMapCenter.y()))
    {
        char tempFileName[100];
        std::string gridmap_file_path = gridmap_path;
        //grid map file name
        gridmap_file_path.append("/%i_%i.png");
        sprintf(tempFileName,gridmap_file_path.c_str(),(int)curGridMapCenter.x(), (int)curGridMapCenter.y());
        //if get grid map file
        if(myLoadFromBitmapFile(tempFileName,
                                gridMap_resolution,
                                (gridMap_halfSize-curGridMapCenter.x())/gridMap_resolution,
                                (gridMap_halfSize-curGridMapCenter.y())/gridMap_resolution))
        {
            ROS_INFO("Map loaded: %f, %f", gridMap_halfSize - curGridMapCenter.x(),
                     gridMap_halfSize - curGridMapCenter.y());
        }
        //if doesn't get grid map file, use loaded global point cloud to generate grid map
        else if(GENERATE_GRIDMAP && LOAD_POINTCLOUD)
        {
            CTicTac gridTic;
            gridTic.Tic();
            localPointsMap.clear();
            globalPointsMap.extractPoints(
                CPoint3D(curGridMapCenter.x() - gridMap_halfSize + gridMap_resolution*2,
                         curGridMapCenter.y() - gridMap_halfSize + gridMap_resolution*2,
                         pointsMap_heightMin),
                CPoint3D(curGridMapCenter.x() + gridMap_halfSize - gridMap_resolution*2,
                         curGridMapCenter.y() + gridMap_halfSize - gridMap_resolution*2,
                         pointsMap_heightMax),
                &localPointsMap);

            float xmax = curGridMapCenter.x() + gridMap_halfSize;
            float xmin = curGridMapCenter.x() - gridMap_halfSize;
            float ymax = curGridMapCenter.y() + gridMap_halfSize;
            float ymin = curGridMapCenter.y() - gridMap_halfSize;

            //if max & min calculation error
            if(!((xmax>xmin)&&(ymax>ymin)))
            {
                ROS_INFO("setSize error, xmax %f, xmin %f, ymax %f, ymin %f, posex %f, posey %f",
                         xmax,xmin,ymax,ymin,poseEst2D.x(),poseEst2D.y());
            }
            else{
                localGridMap.setSize(xmin,xmax,ymin,ymax, gridMap_resolution,1);
                for (int i = 0; i < spmGridMap_m; i++)
                {
                    for (int j = 0; j < spmGridMap_n; j++)
                    {
                        spmLocalGridMap[i][j] = 0;
                        spmLocalGridMap_max[i][j] = 0;
                        spmLocalGridMap_min[i][j] = 0;
                    }
                }
               
                std::vector<float> tx, ty, tz;
                localPointsMap.getAllPoints(tx, ty, tz);
                int lpmSize = tx.size();   //local points map size(points number)
                int xx, yy;
                for (int i = 0; i < lpmSize; i++)
                {
                    xx = localGridMap.x2idx(tx[i]);
                    yy = localGridMap.y2idx(ty[i]);
                    spmLocalGridMap[xx][yy] += 1;
                    if(tz[i]>spmLocalGridMap_max[xx][yy]){spmLocalGridMap_max[xx][yy] = tz[i];}
                    else if(tz[i]<spmLocalGridMap_min[xx][yy]){spmLocalGridMap_min[xx][yy] = tz[i];}
                }

                float gap, density, tempValue;
                float cx, cy;
                int i, j;
                for (i = 0; i < spmGridMap_m; i++)
                {
                    for (j = 0; j < spmGridMap_n; j++)
                    {
                        //counting the points in the cell
                        if (spmLocalGridMap[i][j] < gridMap_cellPointsThreshold) continue;
                        gap = spmLocalGridMap_max[i][j];//-spmLocalGridMap_min[i][j];
                        if(!gap) continue;
                        else density = (float)spmLocalGridMap[i][j];//density here more like quantity
                        tempValue = 1 - gap*gridMap_pzFactor-density*gridMap_enhanceStep;//i feel sorry for this
                        if(tempValue<0) localGridMap.setCell(i,j,0);
                        else localGridMap.setCell(i,j,tempValue);
                    }//end j
                }//end i
                //save grid map file to image
                if(GENERATE_GRIDMAPFILE){
                    char occupancyMapFileName[100];
                    std::string gridmap_file_path_temp = gridmap_path;
                    gridmap_file_path_temp.append("/%i_%i.png");
                    sprintf(occupancyMapFileName,
                            gridmap_file_path_temp.c_str(),
                            (int)curGridMapCenter.x(),
                            (int)curGridMapCenter.y());
                    localGridMap.saveAsBitmapFile(occupancyMapFileName);
                    ROS_INFO("grid map file saved");
                }
            }
            ROS_INFO("grid map generated in %.3fms",gridTic.Tac()*1000);
        }//end of calculate grid map local
        //neither get grid map file nor generate grid map from global point cloud
        else {
            //generate grid map from local point cloud?
        }
    }//end of else

    /*-----------------------------------Map mathcing---------------------------------------*/
    //if (LOAD_POINTCLOUD && icpStarted && !USE_GPSFORMAPPING)
    if (icpStarted && !USE_GPSFORMAPPING)
    {
        //use gps for initialGuess
        if(DR.notDoingIcpYet)
        {
            initialGuess = poseEkf2D;
            DR.notDoingIcpYet = false;

            //DR.glResult_EKF initialization
            Lu_Matrix X0 = Lu_Matrix(3,1);
            Lu_Matrix P0 = Lu_Matrix(3,3);
            X0(0,0)=poseEkf2D.x();
            X0(1,0)=poseEkf2D.y();
            X0(2,0)=poseEkf2D.phi();
            if(X0(2,0)<0) X0(2,0) += 2*PI;
            P0(0,0)=1;
            P0(1,1)=1;
            P0(2,2)=0.1;
            DR.glResult_EKF.init(3,X0,P0);//glResult_EKF(ekf result of matching result) init
        }
        //use motion model(odo and imu) for initialGuess
        else{
            initialGuess = poseEst2D_last + poseIncr2D;// + random to test matcher;
        }

        pdfG = CPosePDFGaussian(initialGuess);//

        //use grid map as reference map
        if(!USE_GLOBALPOINTCLOUDFORMATCHING)
        {
            CTicTac icp_tic;    //timer
            icp_tic.Tic();  //timer start
            CPosePDFPtr pdf = icp.AlignPDF(
                &localGridMap,			// Reference map //localPointsMap
                &curPointsMapColored,	// Map to be aligned
                pdfG,                   // initial estimation
                &runningTime, 
                (void*)&info
            );
            icp_tic_time = icp_tic.Tac()*1000;   //timer end, unit s->ms
            poseEst2D = pdf->getMeanVal();
            pdf->getCovarianceAndMean(covariance_matching, poseEst2D);
            hasCurRobotPoseEst = true;
        }

        //use point cloud map as reference map
        else
        {
            if(!LOAD_POINTCLOUD) ROS_INFO("ERROR, use global point map for matching need LOAD_POINTCLOUD=true!");
            localPointsMap.clear();
            globalPointsMap.extractPoints(
                    CPoint3D(curGridMapCenter.x() - gridMap_halfSize+0.2,
                             curGridMapCenter.y() - gridMap_halfSize+0.2,
                             pointsMap_heightMin),
                    CPoint3D(curGridMapCenter.x() + gridMap_halfSize-0.2,
                             curGridMapCenter.y() + gridMap_halfSize-0.2,
                             pointsMap_heightMax),
                    &localPointsMap);

            CTicTac icp_tic;    //timer
            icp_tic.Tic();      //timer start
            CPosePDFPtr pdf = icp.AlignPDF(
                &localPointsMap,			// Reference map
                &curPointsMapColored,		// Map to be aligned
                pdfG
            );				// Starting estimate
            icp_tic_time = icp_tic.Tac()*1000;   //timer end, unit s->ms
            poseEst2D = pdf->getMeanVal();
            hasCurRobotPoseEst = true;
        }

        GLOBV_RO(0,0) = sqrt((double)covariance_matching(0,0));
        GLOBV_RO(1,1) = sqrt((double)covariance_matching(1,1));
        GLOBV_RO*=GLOBV_RO;

        DR.glResult_EKF.Obv_GPS_update(poseEst2D.x(),poseEst2D.y(),GLOBV_RO);
        Lu_Matrix state = DR.glResult_EKF.getState();

        //output matching result or filtered result
        if(OUTPUT_FILTERED_POSE) poseEst2D = CPose2D(state(0,0),state(1,0),state(2,0));
    }
    //if excute_mode==0 || excute_mode==1, use gps ekf result instead of matching result
    else if(SAVE_POINTCLOUD || USE_GPSFORMAPPING) poseEst2D = poseEkf2D;

    CPoint2D pointTemp(poseEst2D.x(),poseEst2D.y());
    poseEstDist2poseEKF = pointTemp.distance2DTo(poseEkf2D.x(),poseEkf2D.y());

    /*--------------------------------Save result log------------------------------------*/
    if(SAVE_RESULTANDLOG)
    {
        outputFile_result.printf("%.2f\t%.2f\t%.2f\t%.5f\t%.2f\t%.2f\t%.5f\t%.3f\t%.2f\n",
            DR.mileage_sum,
            poseEkf2D.x()+output_pose_shift_x,
            poseEkf2D.y()+output_pose_shift_y,
            poseEkf2D.phi(),
            poseEst2D.x()+output_pose_shift_x,
            poseEst2D.y()+output_pose_shift_y,
            poseEst2D.phi(), 
            mainLoop.Tac()*1000,
            info.goodness);
    }

    /*----------------------------Publish localization result----------------------------*/
    { 
        std_msgs::Float64MultiArray pose_vlp16;
        double outLat, outLon;
        DR.xy2latlon(poseEst2D.x(),poseEst2D.y(), outLat, outLon);//xy2latlon

        pose_vlp16.data.push_back(poseEst2D.x()+output_pose_shift_x);
        pose_vlp16.data.push_back(poseEst2D.y()+output_pose_shift_y);
        pose_vlp16.data.push_back(poseEst2D.phi());
        pose_vlp16.data.push_back(info.goodness);       //matching goodness
        pose_vlp16.data.push_back(info.quality);

        pose_pub.publish(pose_vlp16);
    }

    /*-------------------------------3D window display-----------------------------------*/
    if(SHOW_WINDOW3D)
    {
        scene = COpenGLScene::Create();
        scene->getViewport()->setCustomBackgroundColor(TColorf(0,0.1,0.2));
        scene->insert(gridPlane);

        if(SHOW_GRIDMAPLOCAL)
        {
            objGridMap = CSetOfObjects::Create();
            localGridMap.getAs3DObject(objGridMap);
            objGridMap->setColorA(0.7);
            objGridMap->setLocation(CPoint3D(0,0,-0.01));
            scene->insert(objGridMap);
        }

        if(SHOW_GLOBALPOINTCLOUD)
        {
            global_point_cloud = CPointCloud::Create();
            //global_point_cloud->setColor(0.5,0.5,0.5);
            global_point_cloud->enableColorFromZ(1);
            global_point_cloud->loadFromPointsMap(&globalPointsMap);
            scene->insert(global_point_cloud);
        }

        if(SHOW_POINTCLOUDCOLORED)
        {
            //if(!USE_GLOBALPOINTCLOUDFORMATCHING)
            {
                vector<float> point_x, point_y, point_z;
                mrpt::opengl::CPointCloudColouredPtr pointCloudDisplay =
                        mrpt::opengl::CPointCloudColoured::Create();
                pointCloudDisplay->loadFromPointsMap(&curPointsMapColored);
                curPointsMapColored.getAllPoints(point_x, point_y, point_z);
                int colorCode = 0;
                float darkness = 0.15;
                float limitHigh = pointsMap_heightMax;
                float limitLow = pointsMap_heightMin;
                float limitRange = limitHigh - limitLow;
                for (int i = 0; i<point_z.size(); i++)
                {
                    if (point_z[i]>limitHigh) colorCode = 0;
                    else if (point_z[i] < limitLow) colorCode = 1280;
                    else colorCode = (1 - (point_z[i] - limitLow) / limitRange) * 1280;
                    int rangeNum = colorCode / 256;
                    float colorNum = (colorCode % 256) / 256.0;
                    switch (rangeNum)
                    {
                        default:
                        case 0:
                            pointCloudDisplay->setPointColor_fast(i, 1, colorNum*(1-darkness)+darkness, 0+darkness);
                            break;//G(0->255)
                        case 1:
                            pointCloudDisplay->setPointColor_fast(i, (1 - colorNum)*(1-darkness)+darkness, 1, 0+darkness);
                            break;//R(255->0)
                        case 2:
                            pointCloudDisplay->setPointColor_fast(i, 0+darkness, 1, colorNum*(1-darkness)+darkness);
                            break;//B(0->255)
                        case 3:
                            pointCloudDisplay->setPointColor_fast(i, 0+darkness, (1 - colorNum)*(1-darkness)+darkness, 1);
                            break;//G(255->0)
                        case 4:
                            pointCloudDisplay->setPointColor_fast(i, colorNum*(1-darkness)+darkness, 0+darkness, 1);
                            break;//R(0->255)
                        case 5:
                            pointCloudDisplay->setPointColor_fast(i, 0+darkness, 0+darkness, 1);
                            break;
                    }
                }
                pointCloudDisplay->setPointSize(1.3);
                pointCloudDisplay->setPose(poseEst2D);
                scene->insert(pointCloudDisplay);
            }
        }

        if(SHOW_ROBOTPOSE)
        {
            objAxis->setPose(poseEst2D);
            objAxis->setScale(2);
            scene->insert(objAxis);
        }

        if(SHOW_ROBOTMODEL)
        {

        }

        if(SHOW_GROUNDTRUTHPATH)
        {
            CSetOfLinesPtr objPath = CSetOfLines::Create();
            objPath->appendLine(poseEkf2D_last.x(),poseEkf2D_last.y(),0,
                                poseEkf2D.x(),poseEkf2D.y(),0);
            poseEkf2D_last = poseEkf2D;
            objPath->setLineWidth(2);
            objPath->setColor(0.5,0.9,0.5);
            objSetPath->insert(objPath);
            scene->insert(objSetPath);
        }

        if(SHOW_ROBOTPATH)
        {
            if(!(poseEst2D_last.x()==0 && poseEst2D_last.y()==0))
            {
                if (isPoseEstGood)
                {
                    if (!(abs(poseEst2D_last.x() - poseEst2D.x())>3 || abs(poseEst2D_last.y() - poseEst2D.y())>3))
                    {
                        CSetOfLinesPtr objPath = CSetOfLines::Create();
                        objPath->appendLine(poseEst2D_last.x(),poseEst2D_last.y(),0,
                                            poseEst2D.x(),poseEst2D.y(),0);
                        objPath->setLineWidth(2);

                        //if(poseEstDist2poseEKF<(poseErrorMax/2))
                        //  errorColor = TColorf((poseEstDist2poseEKF*2/poseErrorMax)*0.8,0.8,0);
                        //else errorColor = TColorf(0.8,
                        //                          (1-(2*poseEstDist2poseEKF/poseErrorMax))*0.8,//make it darker
                        //                          0);
                        //  objPath->setColor(errorColor);
                        if(poseEstDist2poseEKF<=0.05) errorColor = TColorf(0,0.4,0.9);
                        else if(poseEstDist2poseEKF<=0.1) errorColor = TColorf(0,0.9,0);
                        else if(poseEstDist2poseEKF<=0.2) errorColor = TColorf(0.9,0.9,0);
                        else if(poseEstDist2poseEKF<=0.3) errorColor = TColorf(0.9,0.4,0);
                        else errorColor = TColorf(0.9,0,0);
                        objPath->setColor(errorColor);
                        if(DR.velocity>0.001)objSetPath->insert(objPath);//insert path when moving
                    }
                }
                else {
                    if (!(abs(poseEst2D_last.x() - poseEst2D.x())>3 || abs(poseEst2D_last.y() - poseEst2D.y())>3))
                    {
                        CSetOfLinesPtr objPath = CSetOfLines::Create();
                        objPath->appendLine(poseEst2D_last.x(),poseEst2D_last.y(),0,
                                            poseEst2D.x(),poseEst2D.y(),0);
                        objPath->setLineWidth(2);
                        objPath->setColor(0.2,0.2,1);
                        objSetPath->insert(objPath);
                        if(DR.velocity>0.001)objSetPath->insert(objPath);//insert path when moving
                    }
                }
            }
            scene->insert(objSetPath);        
        }

        if(SHOW_GPSEKFPOSE)
        {
            objGpsEKF->setPose(poseEkf2D);
            objGpsEKF->setScale(2);
            scene->insert(objGpsEKF);
        }

        if(SHOW_MINIWINDOW)
        {
            COpenGLViewportPtr view_mini = scene->createViewport("view_mini");
            view_mini->setBorderSize(1);
            view_mini->setViewportPosition(0.75, 0.75, 0.24, 0.24);
            view_mini->setTransparent(false);
            view_mini->setCustomBackgroundColor(TColorf(0.1,0.1,0.1));//background color of mini window
            //mini map view
            mrpt::opengl::CCamera &camMini = view_mini->getCamera();
            camMini.setZoomDistance(1.5);
            camMini.setAzimuthDegrees(-90);
            camMini.setElevationDegrees(90);
            camMini.setPointingAt(poseEkf2D.x(),poseEkf2D.y(),0);
            //camMini.setPointingAt(poseEst2D.x(),poseEst2D.y(),0);

            mrpt::opengl::CSetOfLinesPtr objPath = CSetOfLines::Create();
            objPath->appendLine(poseEst2D.x(),poseEst2D.y(),0,
                                poseEkf2D.x(),poseEkf2D.y(),0);
            objPath->setLineWidth(2);
            objPath->setColor(1,1,0);

            char error[50];
            sprintf(error,
                    "%.2f m\n%.2f degree",
                    poseEstDist2poseEKF,
                    abs(poseEkf2D.phi()-poseEst2D.phi())/PI*180);
            const string msg_error = error;

            opengl::CText3DPtr obj_text_3d = opengl::CText3D::Create();
            obj_text_3d->setString(msg_error);
            obj_text_3d->setScale(0.08);
            obj_text_3d->setColor(1,1,0);
            obj_text_3d->setPose(CPose2D(poseEkf2D.x()-0.6,poseEkf2D.y()-0.2,0));
            view_mini->insert( obj_text_3d );

            //view_mini->insert(grid_plane_xy);
            view_mini->insert(objPath);
            view_mini->insert(objAxis);
            view_mini->insert(objGpsEKF);
        }

        if(CAMERA_FOLLOW_ROBOT)
        {
            win3D.setCameraPointingToPoint((float)poseEst2D.x(),(float)poseEst2D.y(),0);
        }

        if(SHOW_GRIDBOXS)
        {
            scene->insert(objBoxes);              
        }

        double outLat, outLon;
        DR.xy2latlon(poseEst2D.x(),poseEst2D.y(), outLat, outLon);

        //show info on window 3D
        char win3DMsgState[200];
        sprintf(win3DMsgState,
                "X Y Heading: %.2f %.2f %.2f\nLat Lon: %.6f %.6f\n\nCov: %.6f %.6f %.6f\nCorrespondences ratio: %.3f:\nMatching time:%.1f ms\n\nSpeed: %.2f m/s\nMain proc time: %.1f ms",
                poseEst2D.x()+output_pose_shift_x, poseEst2D.y()+output_pose_shift_x, poseEst2D.phi(),
                outLat, outLon,
                (float)covariance_matching(0,0), (float)covariance_matching(1,1), (float)covariance_matching(2,2),
                info.goodness,
                icp_tic_time,
                DR.velocity,
                mainLoop.Tac()*1000);
        const string textMsgState = win3DMsgState;
        win3D.addTextMessage(0.05,0.95,textMsgState,TColorf(1,1,0),0,MRPT_GLUT_BITMAP_HELVETICA_12);

        opengl::COpenGLScenePtr &ptrScene = win3D.get3DSceneAndLock();
        ptrScene = scene;
        win3D.unlockAccess3DScene();
        win3D.forceRepaint();
    }
}//end of velodyne_points callback


/*---------------------------------------------------------------------------
                    Choose grid map according to poseEst
----------------------------------------------------------------------------*/
bool pointcloud_receive::isGlobalGridMapCenterChange(double robot_x, double robot_y, double Center_x, double Center_y)
{
    int x, y;
    if (robot_x > 0) {
        x = int(robot_x + 10);
        x = floor(x / 20) * 20;
    }
    else {
        x = int(robot_x - 10);
        x = ceil(x / 20) * 20;
    }
    if (robot_y > 0){
        y = int(robot_y + 10);
        y = floor(y / 20) * 20;
    }
    else {
        y = int(robot_y - 10);
        y = ceil(y / 20) * 20;
    }

    if (x != Center_x || y != Center_y){
        curGridMapCenter = CPoint2D(x, y);
        return true;
    }
    else {
        return false;
    }
}


/*---------------------------------------------------------------------------
               Load image, change to COccupancyGridmap2D
----------------------------------------------------------------------------*/
bool  pointcloud_receive::myLoadFromBitmapFile(
    const std::string   &file,
    float           resolution,
    float           xCentralPixel,
    float           yCentralPixel)
{
    CImage      imgFl;
    if (!imgFl.loadFromFile(file,0)) return false;
    return myLoadFromBitmap(imgFl,resolution, xCentralPixel, yCentralPixel);
}


bool  pointcloud_receive::myLoadFromBitmap(const mrpt::utils::CImage &imgFl, float resolution, float xCentralPixel, float yCentralPixel)
{
    size_t bmpWidth = imgFl.getWidth();
    size_t bmpHeight = imgFl.getHeight();
    //Resize grid
    float new_x_max = (imgFl.getWidth() - xCentralPixel) * resolution;
    float new_x_min = - xCentralPixel * resolution;
    float new_y_max = (imgFl.getHeight() - yCentralPixel) * resolution;
    float new_y_min = - yCentralPixel * resolution;

    if((new_x_max>new_x_min) && (new_y_max>new_y_min)){
        localGridMap.setSize(new_x_min,new_x_max,new_y_min,new_y_max,resolution);
        //load cells content
        for (size_t x=0;x<bmpWidth;x++)
        {
            for (size_t y=0;y<bmpHeight;y++)
            {
                float f = imgFl.getAsFloat(x,bmpHeight-1-y);
                f = std::max(0.01f,f);
                f = std::min(0.99f,f);
                localGridMap.setCell((int)x,(int)y,f);
            }
        }
        return true;
    }
    else return false;
}

bool pointcloud_receive::isNan(float fN)
{
    return !(fN==fN);
}


/*---------------------------------------------------------------------------
                           Load configuration
----------------------------------------------------------------------------*/
void pointcloud_receive::grid_localization_init()
{
    CConfigFile configFile;

    switch(excute_mode)
    {
        case 0:
            config_file_path.append("/config_Save_Pointcloud.ini");
            ROS_INFO("EXCUTE_MODE = SAVE_POINT_CLOUD");
            break;
        case 1:
            config_file_path.append("/config_Generate_GridMap.ini");
            ROS_INFO("EXCUTE_MODE = GENERATE_AND_SAVE_GRIDMAP");
            break;
        case 2:
            config_file_path.append("/config_Localization.ini");
            ROS_INFO("EXCUTE_MODE = LOCALIZATION");
            break;
        default:
            ROS_INFO("Wrong EXCUTE_MODE, should be 0,1,2, check launch file");
    }
    configFile.setFileName(config_file_path);

    SHOW_WINDOW3D           = configFile.read_bool("BasicSettings","SHOW_WINDOW3D",1,false);
    SHOW_MINIWINDOW         = configFile.read_bool("BasicSettings","SHOW_MINIWINDOW",0,false);
    SHOW_CURRENTLASERPOINTS = configFile.read_bool("BasicSettings","SHOW_CURRENTLASERPOINTS",0,false);
    SHOW_POINTCLOUD         = configFile.read_bool("BasicSettings","SHOW_POINTCLOUD",0,false);
    SHOW_POINTCLOUDCOLORED  = configFile.read_bool("BasicSettings","SHOW_POINTCLOUDCOLORED",1,false);
    SHOW_LASERPOINTS        = configFile.read_bool("BasicSettings","SHOW_LASERPOINTS",0,false);
    SHOW_GRIDMAPLOCAL       = configFile.read_bool("BasicSettings","SHOW_GRIDMAPLOCAL",0,false);
    SHOW_GRIDBOXS           = configFile.read_bool("BasicSettings","SHOW_GRIDBOXS",0,false);
    SHOW_ROBOTPOSE          = configFile.read_bool("BasicSettings","SHOW_ROBOTPOSE",1,false);
    SHOW_ROBOTPATH          = configFile.read_bool("BasicSettings","SHOW_ROBOTPATH",0,false);
    SHOW_ROBOTMODEL         = configFile.read_bool("BasicSettings", "SHOW_ROBOTMODEL", 0, false);
    SHOW_GROUNDTRUTHPATH    = configFile.read_bool("BasicSettings","SHOW_GROUNDTRUTHPATH",0,false);
    SHOW_GPSEKFPOSE         = configFile.read_bool("BasicSettings","SHOW_GPSEKFPOSE",0,false);
    SHOW_GLOBALPOINTCLOUD   = configFile.read_bool("BasicSettings","SHOW_GLOBALPOINTCLOUD",0,false);

    USE_GLOBALPOINTCLOUDFORMATCHING = configFile.read_bool("BasicSettings","USE_GLOBALPOINTCLOUDFORMATCHING",1,false);
    SAVE_RESULTANDLOG       = configFile.read_bool("BasicSettings","SAVE_RESULTANDLOG",1,false); ROS_INFO("SAVE_RESULTANDLOG = %d",SAVE_RESULTANDLOG);
    SAVE_POINTCLOUD         = configFile.read_bool("BasicSettings","SAVE_POINTCLOUD",0,false); ROS_INFO("SAVE_POINTCLOUD = %d",SAVE_POINTCLOUD);
    SAVE_POINTCLOUDSTEP     = configFile.read_int("BasicSettings", "SAVE_POINTCLOUDSTEP",1,false);
    LOAD_POINTCLOUD         = configFile.read_bool("BasicSettings","LOAD_POINTCLOUD",0,false); ROS_INFO("LOAD_POINTCLOUD = %d",LOAD_POINTCLOUD);
    USE_GPSFORMAPPING       = configFile.read_bool("BasicSettings","USE_GPSFORMAPPING",0,false); ROS_INFO("USE_GPSFORMAPPING = %d",USE_GPSFORMAPPING);
    CAMERA_FOLLOW_ROBOT     = configFile.read_bool("BasicSettings","CAMERA_FOLLOW_ROBOT",1,false);
    GENERATE_GRIDMAP        = configFile.read_bool("BasicSettings","GENERATE_GRIDMAP",0,false);
    GENERATE_GRIDMAPFILE    = configFile.read_bool("BasicSettings","GENERATE_GRIDMAPFILE",0,false);

    OUTPUT_FILTERED_POSE    = configFile.read_bool("BasicSettings","OUTPUT_FILTERED_POSE",1,false);

    output_pose_shift_x     = configFile.read_float("BasicSettings","output_pose_shift_x",0,false);
    output_pose_shift_y     = configFile.read_float("BasicSettings","output_pose_shift_y",0,false);

    whichGpsForInitial      = configFile.read_int("BasicSettings","whichGpsForInitial",1,false);
    timeForGps              = configFile.read_float("BasicSettings","timeForGps",20,false);
    gpsInitialStableCounter = configFile.read_float("BasicSettings","gpsInitialStableCounter",50,false);

    minDisBetweenLaserPoints = configFile.read_float("BasicSettings","minDisBetweenLaserPoints",0.05,false);
    pointsMap_heightMin     = configFile.read_float("BasicSettings","pointsMap_heightMin",0,false);
    pointsMap_heightMax     = configFile.read_float("BasicSettings","pointsMap_heightMax",1.5,false);
    pointsMap_clipOutOfRange= configFile.read_float("BasicSettings","pointsMap_clipOutOfRange",40,false);
    pointsMap_clipCenterDistance = configFile.read_float("BasicSettings","pointsMap_clipCenterDistance",10,false);

    gridMap_enhanceStep     = configFile.read_float("BasicSettings","gridMap_enhanceStep",0.01,false);
    gridMap_pzFactor        = configFile.read_float("BasicSettings","gridMap_pzFactor",0.1,false);
    gridMap_resolution      = configFile.read_float("BasicSettings","gridMap_resolution",0.1,false);
    gridMap_cellPointsThreshold = configFile.read_float("BasicSettings","gridMap_cellPointsThreshold",10,false);
    gridMap_halfSize        = configFile.read_float("BasicSettings","gridMap_halfSize",40,false);

    pose_front_VLP16_x       = configFile.read_float("BasicSettings","pose_front_VLP16_x",0,false);
    pose_front_VLP16_y       = configFile.read_float("BasicSettings","pose_front_VLP16_y",0.56,false);
    pose_front_VLP16_z       = configFile.read_float("BasicSettings","pose_front_VLP16_z",2.045,false);
    pose_front_VLP16_yaw     = configFile.read_float("BasicSettings","pose_front_VLP16_yaw",0,false);
    pose_front_VLP16_pitch   = configFile.read_float("BasicSettings","pose_front_VLP16_pitch",0.02,false);
    pose_front_VLP16_roll    = configFile.read_float("BasicSettings","pose_front_VLP16_roll",0.01,false);

    pose_rear_VLP16_x       = configFile.read_float("BasicSettings","pose_rear_VLP16_x",0,false);
    pose_rear_VLP16_y       = configFile.read_float("BasicSettings","pose_rear_VLP16_y",0.56,false);
    pose_rear_VLP16_z       = configFile.read_float("BasicSettings","pose_rear_VLP16_z",2.045,false);
    pose_rear_VLP16_yaw     = configFile.read_float("BasicSettings","pose_rear_VLP16_yaw",0,false);
    pose_rear_VLP16_pitch   = configFile.read_float("BasicSettings","pose_rear_VLP16_pitch",0.02,false);
    pose_rear_VLP16_roll    = configFile.read_float("BasicSettings","pose_rear_VLP16_roll",0.01,false);

    poseErrorMax    = configFile.read_float("BasicSettings","poseErrorMax", 5, false);
    icp_goodness_threshold = configFile.read_float("BasicSettings", "icp_goodness_threshold", 0.1, false);

    //icp_maxIterations = configFile.read_int("BasicSettings", "icp_maxIterations", 2,false);

    icp.options.loadFromConfigFile(configFile, "ICP");
    //icp.options.maxIterations = icp_maxIterations;

    //if use GPS for point cloud mapping
    if(SAVE_POINTCLOUD) LOAD_POINTCLOUD = false;
    //grid map mapping(already have point cloud, but doesn't have grid map)
    if(USE_GPSFORMAPPING) {
        SAVE_POINTCLOUD = false;
        LOAD_POINTCLOUD = true;
    }

    step = 0;   //global counter
    is_icp_gn_high = false;
    DR.whichGpsForInitial = whichGpsForInitial;
    DR.notDoingIcpYet = true;
    firstTimeShowGps = true;
    odoTemp = 0;
    icpStarted = false;
    centerX = 0;//42500;//42000
    centerY = 0;//2700;//2700
    CPoint2D gridPlaneCenter = CPoint2D(centerX, centerY);//center of grid plane
    poseEstDist2poseEKF = 0;
    gpsEkfDown = false;

    GLOBV_RO = Lu_Matrix(2,2);//
    X0_init = Lu_Matrix(3,1);
    P0_init = Lu_Matrix(3,3);
    X0_init(0,0)=0;
    X0_init(1,0)=0;
    X0_init(2,0)=0;
    P0_init(0,0)=1;
    P0_init(1,1)=1;
    P0_init(2,2)=0.1;

    /*---------------------win3D display initialization-------------------------*/
    if(SHOW_WINDOW3D)
    {
        win3D.setWindowTitle("CyberFly_View");
        win3D.resize(1280, 720);
        win3D.setCameraAzimuthDeg(270);//方向沿y轴由负方向向正方向看
        win3D.setCameraElevationDeg(90);//俯角20°
        win3D.setCameraPointingToPoint(0, 0, 0);
        win3D.setCameraZoom(150);
    }

    //
    initialGuessStableCounter = 0;
    initialGuess = CPose2D(0,0,0);
    hasCurRobotPoseEst = false;
    isPoseEstGood = true;

    /*---------------------points map configuration----------------------*/
    //CSimplePointsMap and CPointsMapColored initialization
    globalPointsMap.enableFilterByHeight(true);
    globalPointsMap.setHeightFilterLevels(pointsMap_heightMin, pointsMap_heightMax);
    globalPointsMap.insertionOptions.minDistBetweenLaserPoints = minDisBetweenLaserPoints;
    localPointsMap.enableFilterByHeight(true);
    localPointsMap.setHeightFilterLevels(pointsMap_heightMin, pointsMap_heightMax);
    localPointsMap.insertionOptions.minDistBetweenLaserPoints = minDisBetweenLaserPoints;
    curPointsMapColored.enableFilterByHeight(true);
    curPointsMapColored.setHeightFilterLevels(pointsMap_heightMin,pointsMap_heightMax);
    curPointsMapColored.insertionOptions.minDistBetweenLaserPoints = minDisBetweenLaserPoints;
    localPointsMapColored.enableFilterByHeight(true);
    localPointsMapColored.setHeightFilterLevels(pointsMap_heightMin,pointsMap_heightMax);
    localPointsMapColored.insertionOptions.minDistBetweenLaserPoints = minDisBetweenLaserPoints;
    globalPointsMapColored.enableFilterByHeight(true);
    globalPointsMapColored.setHeightFilterLevels(pointsMap_heightMin,pointsMap_heightMax);
    globalPointsMapColored.insertionOptions.minDistBetweenLaserPoints = minDisBetweenLaserPoints;
    //a point in front of the vehicle, as the center of local point cloud (for clipOutOfRange())
    clipCenter = CPoint2D(0,0);
    curGridMapCenter = CPoint2D(0,0);
    dFromPoseToCenter = 20;

    timPrev = ros::Time::now().toSec();

    poseDR2D = CPose2D(0,0,0);
    poseDR3D = CPose3D(0,0,0);
    //poseEst2D = CPose2D(-21729,66191,-0.9);
    poseEst2D = CPose2D(centerX,centerY,0);

    /*--------------------elements in 3D window initialization---------------------*/
    //grid plane
    gridPlane = CGridPlaneXY::Create(-1000+centerX,1000+centerX,-1000+centerY,1000+centerY,0.01,10,0.5);
    gridPlane->setColor(0.375,0.375,0.375);
    //axis for estimated pose (grid localization pose)
    objAxis = opengl::stock_objects::CornerXYZSimple(2,2);
    objAxis->setName("poseEst");
    objAxis->enableShowName(true);
    //axis for ground truth pose (gps pose)
    objGpsEKF = opengl::stock_objects::CornerXYZSimple(2,2);
    objGpsEKF->setName("GT");
    objGpsEKF->enableShowName(true);
    //3D text object
    obj_text_ground_truth = mrpt::opengl::CText::Create();
    obj_text_estimated_pose;
    //path object (once) of grid localization result, should be inserted to objSetPath
    objEkfPath = CSetOfLines::Create();
    //path set
    objSetPath = CSetOfObjects::Create();
    //disk
    objDisk = CDisk::Create();
    objDisk->setDiskRadius(30 );
    objDisk->setColor(1,0.2,1);


    /*----------gridmap memory alloc(for generation gridmap)----------*/
    if(GENERATE_GRIDMAP)
    {
        spmGridMap_m = gridMap_halfSize*2 / gridMap_resolution;
        spmGridMap_n = gridMap_halfSize*2 / gridMap_resolution;
        spmLocalGridMap = (int**)malloc(sizeof(int*)* spmGridMap_m); //¿ª±ÙÐÐ
        spmLocalGridMap_max = (float**)malloc(sizeof(float*)* spmGridMap_m);
        spmLocalGridMap_min = (float**)malloc(sizeof(float*)* spmGridMap_m);

        //initial the gridMap correlate counting array
        for (int i = 0; i < spmGridMap_m; i++){
            *(spmLocalGridMap + i) = (int*)malloc(sizeof(int)* spmGridMap_n);//¿ª±ÙÁÐ
            *(spmLocalGridMap_max + i) = (float*)malloc(sizeof(float)* spmGridMap_n);
            *(spmLocalGridMap_min + i) = (float*)malloc(sizeof(float)* spmGridMap_n);
        }
        //erase the gridMap correlate counting array
        for (int i = 0; i < spmGridMap_m; i++){
            for (int j = 0; j < spmGridMap_n; j++){
                spmLocalGridMap[i][j] = 0;
                spmLocalGridMap_max[i][j] = 0;
                spmLocalGridMap_min[i][j] = 0;
            }
        }
    }

    /*----------------------load point cloud file---------------------*/
    //gridMap initialization
    if(LOAD_POINTCLOUD)
    {
        //start timer for point cloud load timing
        tictac.Tic();
        printf("Loading globalPointsMap...\n");
        CSimplePointsMap globalPointsMapTemp;//temp point cloud for generate globalPointsMap
        globalPointsMapTemp.loadFromPlyFile(pointcloud_file);
        float max_x, max_y, max_z, min_x, min_y, min_z;
        globalPointsMapTemp.boundingBox(min_x, max_x, min_y, max_y, min_z, max_z);
        globalPointsMapTemp.extractPoints(
                TPoint3D(min_x, min_y, pointsMap_heightMin),
                TPoint3D(max_x, max_y, pointsMap_heightMax),
                &globalPointsMap);
        //print

        printf("globalPointsMap load in %.3f s.\nglobalPointsMap size: %i points\n", tictac.Tac(), (int)globalPointsMap.size());
        //print memory usage
        memory_usage_pointcloud = (float)getMemoryUsage()/(1024*1024);
        ROS_INFO("Memory usage after initialization: %fMb",memory_usage_pointcloud);
    }

    /*----------------------initialize log file name---------------------*/
    if(SAVE_RESULTANDLOG){
        time_t timep;
        struct tm *p;
        time(&timep);
        p=gmtime(&timep);

        char log_file_name[200];
        sprintf(log_file_name,
                "/%d-%02d-%02d-%02d-%02d-%02d.txt",
                (1900+p->tm_year),(1+p->tm_mon),p->tm_mday,p->tm_hour,p->tm_min,p->tm_sec);
        log_file_path.append(log_file_name);
        outputFile_result.open(log_file_path,0);
    }
    //start timer for main process
    tictac.Tic();

    ROS_INFO("Grid_localization initialization done.");
}
