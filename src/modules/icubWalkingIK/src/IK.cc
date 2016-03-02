#include <IK.h>
#include "UtilityFunctions.h"
#include <yarpWholeBodyInterface/yarpWholeBodyModel.h>
#include <yarpWholeBodyInterface/yarpWholeBodyStates.h>
#include <Eigen/Dense>
#include <Eigen/Core>
#include <Eigen/LU>
#include <Eigen/SVD>
#include <Eigen/Geometry>

//using namespace qpOASES;
//using namespace RigidBodyDynamics;
using namespace Eigen;

void getEulerAngles(Eigen::Matrix3d R, Eigen::Vector3d& angles, std::string order)
{
    if(order.compare("123") == 0)
    {
        angles[0] = atan2(R(1,2),R(2,2));
        angles[1] = -asin(R(0,2));
        angles[2] = atan2(R(0,1),R(0,0));
    } else if(order.compare("321") == 0)
    {
        angles[0] = atan2(-R(1,0),R(0,0));
        angles[1] = asin(R(2,0));
        angles[2] = atan2(-R(2,1),R(2,2));
    }
}


//by Kevin
Eigen::MatrixXd CalcOrientationEulerXYZ (const Eigen::VectorXd &input,
                                         std::string order) {
    Eigen::Matrix3d R1 = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d R2 = Eigen::Matrix3d::Zero();
    Eigen::Matrix3d R3 = Eigen::Matrix3d::Zero();
    
    Eigen::Matrix3d result = Eigen::Matrix3d::Zero();
    
    if(order.compare("123")==0)
    {
        R1 = rotx(input[0]);
        R2 = roty(input[1]);
        R3 = rotz(input[2]);
        result = R1*R2*R3;
    }
    else if(order.compare("321")==0)
    {
        R1 = rotx(input[2]);
        R2 = roty(input[1]);
        R3 = rotz(input[0]);
        result = R3*R2*R1;
    } else
    {
        std::cout << "Order " << order << " not implemented yet!" << std::endl;
        abort();
    }
    
    return result;
}


Eigen::Vector3d CalcAngularVelocityfromMatrix (const Eigen::Matrix3d &RotMat) {
    
    
    float tol = 1e-12;
    
    Eigen::Vector3d l = Eigen::Vector3d (RotMat(2,1) - RotMat(1,2), RotMat(0,2) - RotMat(2,0), RotMat(1,0) - RotMat(0,1));
    if(l.norm() > tol)
    {
        double preFactor = atan2(l.norm(),(RotMat.trace() - 1.0))/l.norm();
        return preFactor*l;
    }
    else if((RotMat(0,0)>0 && RotMat(1,1)>0 && RotMat(2,2) > 0) || l.norm() < tol)
    {
        return Eigen::Vector3d::Zero();
    }
    else
    {
        return Eigen::Vector3d (PI/2*(RotMat(0,0) + 1.0),PI/2*(RotMat(1,1) + 1.0),PI/2*(RotMat(2,2) + 1.0));
    }
}


bool IKinematics (wbi::iWholeBodyModel* wbm,
                  wbi::iWholeBodyStates* wbs,
                  const Eigen::VectorXd &Qinit,
                  const std::vector<unsigned int>& body_id,
                  const std::vector<Eigen::Vector3d>& target_pos,
                  const std::vector<Eigen::Matrix3d>& target_orientation,
                  const std::vector<Eigen::Vector3d>& body_point,
                  Eigen::VectorXd &Qres,
                  double step_tol,
                  double lambda,
                  unsigned int max_iter)
{    
    assert (Qinit.size() == wbm->getDoFs());
    assert (body_id.size() == target_pos.size());
    assert (body_id.size() == body_point.size());
    assert (body_id.size() == target_orientation.size());
    
    Eigen::MatrixXd J = Eigen::MatrixXd::Zero(6 * body_id.size(), wbm->getDoFs());
    Eigen::VectorXd e = Eigen::VectorXd::Zero(6 * body_id.size());
    
    Qres = Qinit;
    
    for (unsigned int ik_iter = 0; ik_iter < max_iter; ik_iter++) {
//        UpdateKinematicsCustom (model, &Qres, NULL, NULL);
        Eigen::VectorXd q_init(wbm->getDoFs());
        wbs->getEstimates(wbi::ESTIMATE_JOINT_POS, q_init.data());
        
        for (unsigned int k = 0; k < body_id.size(); k++) {
            // Initializing Jacobian matrix to 6 x DOFS
            Eigen::MatrixXd G (Eigen::MatrixXd::Zero(6, wbm->getDoFs()));
            wbm->computeJacobian(q_init.data(), wbi::Frame(), body_id[k], G.data());
//            CalcPointJacobian6D (model, Qres, body_id[k], body_point[k], G, false);
            
            // Calculate coordinates of a point in the root reference frame
            Eigen::VectorXd point_pose(7);
            wbm->forwardKinematics(q_init.data(), wbi::Frame(), body_id[k], point_pose.data());
            Eigen::Vector3d point_base = point_pose.head(3);
//            Eigen::Vector3d point_base = CalcBodyToBaseCoordinates (model, Qres, body_id[k], body_point[k], false);
            
            // Calculate orientation of a given body as 3x3 matrix
            Eigen::VectorXd orientationAxisAngle(4);
            orientationAxisAngle = point_pose.tail(4);
            Eigen::AngleAxis<double> aa(orientationAxisAngle(3), Eigen::Vector3d(orientationAxisAngle(0),
                                                                                 orientationAxisAngle(1),
                                                                                 orientationAxisAngle(2)));
            Eigen::Matrix3d R = aa.toRotationMatrix();
//            Eigen::Matrix3d R = CalcBodyWorldOrientation(model, Qres, body_id[k], false);
            
            Eigen::Vector3d ort_rates = Eigen::Vector3d::Zero();
            
            if(!target_orientation[k].isZero(0))
                ort_rates = R.transpose()*CalcAngularVelocityfromMatrix(R*target_orientation[k].transpose());
            
            for (unsigned int i = 0; i < 6; i++) {
                for (unsigned int j = 0; j < wbm->getDoFs(); j++) {
                    unsigned int row = k * 6 + i;
                    J(row, j) = G (i,j);
                }
            }
            for (unsigned int i = 0; i < 3; i++) {
                e[k * 6 + i + 3] = target_pos[k][i] - point_base[i];
                e[k * 6 + i] = ort_rates[i];
            }
        }
        
        // abort if we are getting "close"
        if (e.norm() < step_tol) {
            LOG << "Reached target close enough after " << ik_iter << " steps" << std::endl;
            return true;
        }
        
        double wn = lambda;
        
        // "task space" from puppeteer
        Eigen::MatrixXd Ek = Eigen::MatrixXd::Zero (e.size(), e.size());
        
        for (size_t ei = 0; ei < e.size(); ei ++) {
            Ek(ei,ei) = e[ei] * e[ei] * 0.5 + wn;
        }
        
        Eigen::MatrixXd JJT_Ek_wnI = J * J.transpose() + Ek;
        
        Eigen::VectorXd delta_theta = J.transpose() * JJT_Ek_wnI.colPivHouseholderQr().solve (e);
        
        Qres = Qres + delta_theta;
        if (delta_theta.norm() < step_tol) {
            return true;
        }
    }
    return false;
}