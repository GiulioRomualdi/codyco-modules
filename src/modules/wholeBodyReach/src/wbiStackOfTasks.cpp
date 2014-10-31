/* 
 * Copyright (C) 2013 CoDyCo
 * Author: Andrea Del Prete
 * email:  andrea.delprete@iit.it
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <wholeBodyReach/wbiStackOfTasks.h>
#include <wholeBodyReach/eiquadprog2011.hpp>
#include <wholeBodyReach/Stopwatch.h>
#include <Eigen/SVD>

#include <yarp/sig/Matrix.h>
#include <iCub/ctrl/math.h>
#include <iostream>
#include <cstdio>

using namespace wholeBodyReach;
using namespace paramHelp;
using namespace Eigen;
using namespace std;
using namespace iCub::ctrl;
using namespace yarp::math;


#define Jc_j        _Jc.rightCols(_n)
#define Jc_b        _Jc.leftCols<6>()
#define M_b         _M.topLeftCorner<6, 6>()
#define M_bj        _M.topRightCorner(6,_n)
#define M_u         _M.topRows<6>()
#define M_a         _M.bottomRows(_n)
#define h_b         _h.head<6>()
#define h_j         _h.tail(_n)
#define ddqDes_b    _ddqDes.head<6>()
#define ddqDes_j    _ddqDes.tail(_n)

#define PROFILE_WHOLE_SOLVER            "Whole solver"
#define PROFILE_DYNAMICS_COMPUTATION    "Dynamics computation"
#define PROFILE_FORCE_QP_PREP           "Force QP preparation"
#define PROFILE_FORCE_QP_MOMENTUM       "Force QP momentum task update"
#define PROFILE_FORCE_QP                "Force QP"
#define PROFILE_FORCE_TOTAL             "Force overall computation"
#define PROFILE_DDQ_DYNAMICS_CONSTR     "Compute dynamics-consistent ddq"
#define PROFILE_DDQ_CONTACT_CONSTR      "Compute contact-consistent ddq"
#define PROFILE_DDQ_POSTURE_TASK        "Compute posture task"

//**************************************************************************************************
wbiStackOfTasks::wbiStackOfTasks(wbi::wholeBodyInterface* robot, bool useNullspaceBase)
:   _robot(robot),
    _momentumTask(NULL),
    _postureTask(NULL),
    _jointLimitTask(NULL),
    _useNullspaceBase(useNullspaceBase),
    _ctrlAlg(WBR_CTRL_ALG_MOMENTUM_SOT)
{
    this->useNullspaceBase(_useNullspaceBase==1);
    
    _n = robot->getDoFs();
    _M.resize(_n+6,_n+6);
    _h.resize(_n+6);
    
    _Mb_inv.setZero(6,6);
    _Mb_llt = LLT<MatrixRXd>(6);
    _ddqDes.setZero(_n+6);
    _ddq_jDes.setZero(_n);
    _ddq_jPosture.setZero(_n);
    
    _A_i.setZero(6,_n+6);
    _b_i.setZero(6);

    _qpData_f.CE.resize(0,0);
    _qpData_f.ce0.resize(0);
    _qpData_f.CI.resize(0, 0);
    _qpData_f.ci0.resize(0);
    _qpData_f.activeSetSize   = 0;
    
    _qpData_ddq.activeSetSize = 0;
    _qpData_ddq.H.resize(_n,_n);
    _qpData_ddq.g.resize(_n);
    _qpData_ddq.CE.resize(0,0);
    _qpData_ddq.ce0.resize(0);
    _qpData_ddq.CI.resize(0, 0);
    _qpData_ddq.ci0.resize(0);
}

//**************************************************************************************************
bool wbiStackOfTasks::computeSolution(RobotState& robotState, Eigen::VectorRef torques)
{
    START_PROFILING(PROFILE_WHOLE_SOLVER);
    
    assert(_momentumTask!=NULL);
    assert(_postureTask!=NULL);
    
    useNullspaceBase(_useNullspaceBase==1);
    
    //*********************************
    // COMPUTE ROBOT DYNAMICS
    //*********************************
    START_PROFILING(PROFILE_DYNAMICS_COMPUTATION);
    {
        _robot->computeGeneralizedBiasForces(robotState.qJ.data(), robotState.xBase,
                                             robotState.dqJ.data(), robotState.vBase.data(),
                                             robotState.g.data(), _h.data());
        _robot->computeMassMatrix(robotState.qJ.data(), robotState.xBase, _M.data());
    }
    STOP_PROFILING(PROFILE_DYNAMICS_COMPUTATION);
    
    //************************************************
    // COMPUTE QUANTITIES RELATED TO CONTACT FORCES
    //************************************************
    START_PROFILING(PROFILE_FORCE_QP_PREP);
    {
        int index_k = 0, k=0, index_in=0, in=0;
        for(list<ContactConstraint*>::iterator it=_constraints.begin(); it!=_constraints.end(); it++)
        {
            ContactConstraint& c = **it;
            c.update(robotState);
            
            k  = c.getSize();    // number of constraint forces
            in = c.getNumberOfInequalities();
            
            c.getInequalityMatrix( _qpData_f.CI.block(index_in, index_k, in, k)); // CI = [CI, t.getInequalityMatrix()]
            c.getInequalityVector( _qpData_f.ci0.segment(index_in, in) );         //  b = [b; t.getInequalityVector()]
            c.getMomentumMapping(  _X.middleCols(index_k, k)  );                //  X = [X, t.getMomentumMapping()]
            c.getEqualityMatrix(   _Jc.middleRows(index_k, k) );                // Jc = [Jc; t.getEqualityMatrix()]
            c.getEqualityVector(   _dJcdq.segment(index_k, k) );                // dJc_dq = [dJc_dq; t.getEqualityVector()]
            c.getWeights(          _fWeights.segment(index_k,k));
            
            // if the normal force of this constraint (computed at the last step) is not zero
            // then divide the weights by the normal force
            if(_fcDes(index_k+2)!=0.0)
                _fWeights.segment(index_k,k) /= _fcDes(index_k+2);
            
//            sendMsg("CI block "+c.getName()+":\n"+toString(_qpData_f.CI.block(index_in, index_k, in, k),1,"\n",12));
//            sendMsg("ci0: "+toString(_qpData_f.ci0.segment(index_in, in),1));
            
            index_k += k;
            index_in += in;
        }
        _dJcdq *= -1.0;
        
        assert(index_k==_k);
//        sendMsg("X:\n"+toString(_X,1,"\n",12));
//        sendMsg("CI:\n"+toString(_qpData_f.CI,1,"\n",12));
//        sendMsg("ci0: "+toString(_qpData_f.ci0,1));
        
        START_PROFILING(PROFILE_FORCE_QP_MOMENTUM);
        {
            _momentumTask->update(robotState);
            _momentumTask->getEqualityVector(_momentumDes);
            _fWeights.normalize();
            _W.diagonal() = _fWeights;
        }
        STOP_PROFILING(PROFILE_FORCE_QP_MOMENTUM);
        //    sendMsg("momentumDes "+toString(_momentumDes,1));
        //    cout<< "QP gradient "<<toString(_qpData_f.g,1)<<endl;
    }
    STOP_PROFILING(PROFILE_FORCE_QP_PREP);
    
    // update postural task
    _postureTask->update(robotState);
    _postureTask->getEqualityVector(_ddq_jPosture);
    
    bool res;
    switch(_ctrlAlg)
    {
        case WBR_CTRL_ALG_MOMENTUM_SOT:     res = computeMomentumSoT(robotState, torques);      break;
        case WBR_CTRL_ALG_NULLSPACE_PROJ:   res = computeNullspaceProj(robotState, torques);    break;
        case WBR_CTRL_ALG_COM_POSTURE:      res = computeComPosture(robotState, torques);       break;
        case WBR_CTRL_ALG_MOMENTUM_POSTURE: res = computeMomentumPosture(robotState, torques);  break;
        default:                            printf("WbiStackOfTask: ERROR unmanaged control algorithm!\n");
    }
    
//    sendMsg("tau des: "+jointToString(torques,1));
//    sendMsg("fc des: "+toString(_fcDes,1));
//    sendMsg("Jc^t*f: "+toString(_Jc.transpose()*_fcDes,1));
//    sendMsg("dJc*dq: "+toString(_dJcdq,2));
//    sendMsg("xB:    \n"+robotState.xBase.toString());
//    sendMsg("q:       "+jointToString(WBR_RAD2DEG*robotState.qJ,1));
//    sendMsg("dq:      "+jointToString(WBR_RAD2DEG*robotState.dq,1));
//    sendMsg("ddqDes:  "+jointToString(WBR_RAD2DEG*_ddqDes,2));
//    sendMsg("Torques measured: "+jointToString(robotState.torques));
    
    
    if(_ctrlAlg!=WBR_CTRL_ALG_NULLSPACE_PROJ)
    {
        int index_k = 0, k=0;
        for(list<ContactConstraint*>::iterator it=_constraints.begin(); it!=_constraints.end(); it++)
        {
            ContactConstraint& c = **it;
            k  = c.getSize();    // number of constraint forces
            c.setDesiredConstraintForce(_fcDes.segment(index_k, k));
            index_k += k;
        }
    }
    
    if(_jointLimitTask!=NULL)
        _jointLimitTask->setDdqDes(_ddqDes.tail(_n));
    
    STOP_PROFILING(PROFILE_WHOLE_SOLVER);
    
#ifdef DEBUG_FORWARD_DYNAMICS
    _qj = robotState.qJ;
    _xB = robotState.xBase;
    _dq = robotState.dq;
    
    _ddqFD.resize(_n+6);   // accelerations computed by the forward dynamics algorithm
    constrainedForwardDynamics(robotState.g, torques, robotState.xBase, robotState.qJ, robotState.dq, _ddqFD);
    sendMsg("ddqFD-ddqDes: "+toString((_ddqFD-_ddqDes).norm()));
#endif
    
    return res;
}

//**************************************************************************************************
bool wbiStackOfTasks::computeMomentumSoT(RobotState& robotState, Eigen::VectorRef torques)
{
    sendMsg("ComputeMomentumSoT");
    
    _qpData_f.H   = _X.transpose()*_X + _numericalDampingTask*_W;
    _qpData_f.g   = -_X.transpose()*_momentumDes;
    
    double res;
    START_PROFILING(PROFILE_FORCE_QP);
    {
        res = solve_quadprog(_qpData_f.H, _qpData_f.g, _qpData_f.CE.transpose(), _qpData_f.ce0,
                             - _qpData_f.CI.transpose(), _qpData_f.ci0, _fcDes,
                             _qpData_f.activeSet, _qpData_f.activeSetSize);
    }
    STOP_PROFILING(PROFILE_FORCE_QP);
    if(res == std::numeric_limits<double>::infinity())
        return false;
    
    if(_qpData_f.activeSetSize>0)
        sendMsg("Active constraints: "+toString(_qpData_f.activeSet.head(_qpData_f.activeSetSize).transpose()));
    sendMsg("Momentum error  = "+toString((_X*_fcDes-_momentumDes).norm()));
    
    //*********************************
    // COMPUTE DESIRED ACCELERATIONS
    //*********************************
    
    // Compute ddq that respect dynamics:
    //     ddqBar = [Mb^{-1}*(Jc_u^T*f-h_u); zeros(n,1)];
    START_PROFILING(PROFILE_DDQ_DYNAMICS_CONSTR);
    {
        computeMb_inverse();
        ddqDes_b    = _Mb_inv*(Jc_b.transpose()*_fcDes - h_b);
        ddqDes_j.setZero();
    }
    STOP_PROFILING(PROFILE_DDQ_DYNAMICS_CONSTR);
    
    //    sendMsg("ddqDes (dynamic consistent) = "+toString(_ddqDes,1));
    //    sendMsg("Base dynamics error  = "+toString((M_u*_ddqDes+h_b-Jc_b.transpose()*_fcDes).norm()));
    
    // Compute ddq that respect also contact constraints:
    //      Sbar     = [-Mb^{-1}*Mbj; eye(n)];
    //      ddq_jDes = (Jc*Sbar).solve(-Jc*ddqDes - dJc_dq);
    //      ddqBar  += Sbar*ddq_jDes
    START_PROFILING(PROFILE_DDQ_CONTACT_CONSTR);
    {
        _Mb_inv_M_bj= _Mb_inv * M_bj;
        _Jc_Sbar    = Jc_j - (Jc_b * _Mb_inv_M_bj);
        _Jc_Sbar_svd.compute(_Jc_Sbar, _svdOptions);
        //        cout<<"Jc_b =\n"<< toString(Jc_b,1)<< endl;
        //        cout<<"Jc_b*ddqDes_b = "<< toString(Jc_b*ddqDes_b,1)<< endl;
        //        cout<<"ddqDes_b = "<< toString(ddqDes_b,1)<< endl;
        _ddq_jDes    = svdSolveWithDamping(_Jc_Sbar_svd, -_dJcdq-Jc_b*ddqDes_b, _numericalDampingConstr);
        //        _ddq_jDes    = _Jc_Sbar_svd.solve(-_dJcdq-Jc_b*ddqDes_b);
        ddqDes_b    -= _Mb_inv_M_bj*_ddq_jDes;
        ddqDes_j    += _ddq_jDes;
    }
    STOP_PROFILING(PROFILE_DDQ_CONTACT_CONSTR);
    
    //    sendMsg("ddqDes (contact consistent) = "+toString(_ddqDes,1));
    //    sendMsg("Base dynamics error  = "+toString((M_u*_ddqDes+h_b-Jc_b.transpose()*_fcDes).norm()));
//    sendMsg("Contact constr error = "+toString((_Jc*_ddqDes+_dJcdq).norm()));
    
    // Now we can go on with the motion tasks, using the nullspace of dynamics and contacts:
    //      Z = nullspace(Jc*Sbar);
    _Z.setIdentity(_n,_n);
    updateNullspace(_Jc_Sbar_svd);
    //    sendMsg("Jc*Sbar*Z = "+toString((_Jc_Sbar*_Z).sum()));
    
#define DEBUG_POSE_TASKS
    // Solve the equality motion task
    // N=I
    // for i=1:K
    //      A = A_i*Sbar*N
    //      ddq_j = A^+ (b_i - A_i*ddq)
    //      ddq  += Sbar*ddq_j
    //      N    -= A^+ A
    // end
    bool jointLimitActive = _jointLimitTask!=NULL;
    if(jointLimitActive)
    {
        _jointLimitTask->update(robotState);
    }
    
    double tmp;
    list<MinJerkPDLinkPoseTask*>::iterator it;
    for(it=_equalityTasks.begin(); it!=_equalityTasks.end(); it++)
    {
        MinJerkPDLinkPoseTask& t = **it;
        t.update(robotState);
        
        t.getEqualityMatrix(_A_i);
        t.getEqualityVector(_b_i);
        _A = _A_i.rightCols(_n) - _A_i.leftCols<6>()*_Mb_inv_M_bj;
        _A *= _Z;
        _A_svd.compute(_A, _svdOptions);

        if(jointLimitActive)
        {
            // even if CI is constant, you need to update it every time because we overwrite it
            _jointLimitTask->getInequalityMatrix(_qpData_ddq.CI);
            _jointLimitTask->getInequalityVector(_qpData_ddq.ci0);
            
            _qpData_ddq.H       = _A.transpose()*_A + _numericalDampingTask*MatrixXd::Identity(_n,_n);
            _qpData_ddq.g       = -_A.transpose()*(_b_i - _A_i*_ddqDes);
            _qpData_ddq.ci0     += _qpData_ddq.CI*ddqDes_j;
            _qpData_ddq.CI      *= _Z;
            double res = solve_quadprog(_qpData_ddq.H, _qpData_ddq.g, _qpData_ddq.CE.transpose(), _qpData_ddq.ce0,
                                     _qpData_ddq.CI.transpose(), _qpData_ddq.ci0, _ddq_jDes,
                                     _qpData_ddq.activeSet, _qpData_ddq.activeSetSize);
            if(res == std::numeric_limits<double>::infinity())
                return false;
            if(_qpData_ddq.activeSetSize>0)
                sendMsg(t.getName()+". Active joint limits: "+toString(_qpData_ddq.activeSet.head(_qpData_ddq.activeSetSize).transpose()));
        }
        else
        {
            _ddq_jDes   = svdSolveWithDamping(_A_svd, _b_i - _A_i*_ddqDes, _numericalDampingTask);
        }
        if((tmp=(_ddq_jDes-_Z*_ddq_jDes).norm())>1e-10)
            sendMsg(t.getName()+". Part of ddq_jDes outside the null space of previous tasks: "+toString(tmp));
        ddqDes_b    -= _Mb_inv_M_bj*_ddq_jDes;
        ddqDes_j    += _ddq_jDes;
        updateNullspace(_A_svd);
        
#ifdef  DEBUG_POSE_TASKS
//        sendMsg(t.getName()+" Jacobian =\n"+toString(_A_i));
//        int nzsv = _A_svd.nonzeroSingularValues();
//        sendMsg(t.getName()+". "+toString(nzsv)+"-th sing val = "+toString(_A_svd.singularValues()(nzsv-1)));
//        sendMsg(t.getName()+". Contact constr error = "+toString((_Jc*_ddqDes+_dJcdq).norm()));
//        sendMsg(t.getName()+" ||A*x-b|| =    "+toString((_A_i*_ddqDes-_b_i).norm()));
#endif
    }

    START_PROFILING(PROFILE_DDQ_POSTURE_TASK);
    {
        if(jointLimitActive)
        {
            _jointLimitTask->getInequalityMatrix(_qpData_ddq.CI);
            _jointLimitTask->getInequalityVector(_qpData_ddq.ci0);
            
            _qpData_ddq.H       = _Z + _numericalDampingTask*MatrixXd::Identity(_n,_n);
            _qpData_ddq.g       = -_Z*(_ddq_jPosture - ddqDes_j);
            _qpData_ddq.ci0     += _qpData_ddq.CI*ddqDes_j;
            _qpData_ddq.CI      *= _Z;
            double res = solve_quadprog(_qpData_ddq.H, _qpData_ddq.g, _qpData_ddq.CE.transpose(), _qpData_ddq.ce0,
                                        _qpData_ddq.CI.transpose(), _qpData_ddq.ci0, _ddq_jDes,
                                        _qpData_ddq.activeSet, _qpData_ddq.activeSetSize);
            if(res == std::numeric_limits<double>::infinity())
                return false;
            if(_qpData_ddq.activeSetSize>0)
                sendMsg("Posture task. Active joint limits: "+toString(_qpData_ddq.activeSet.head(_qpData_ddq.activeSetSize).transpose()));
        }
        else
        {
            if(_useNullspaceBase)
                _ddq_jDes   = _Z * (_Z.transpose() * _ddq_jPosture);
            else
                _ddq_jDes   = _Z * _ddq_jPosture;
        }
        
        if((tmp=(_ddq_jDes-_Z*_ddq_jDes).norm())>1e-10)
            sendMsg("Posture task. Part of ddq_jDes outside the null space of previous tasks: "+toString(tmp));
        ddqDes_b    -= _Mb_inv_M_bj*_ddq_jDes;
        ddqDes_j    += _ddq_jDes;
    }
    STOP_PROFILING(PROFILE_DDQ_POSTURE_TASK);
    
    torques     = M_a*_ddqDes + h_j - Jc_j.transpose()*_fcDes;
    
//    sendMsg("fcDes              = "+toString(_fcDes,1));
    //    sendMsg("ddq_jDes     = "+toString(_ddq_jDes,1));
    //    sendMsg("ddq_jPosture        = "+jointToString(_ddq_jPosture,1));
    sendMsg("Base dynamics error  = "+toString((M_u*_ddqDes+h_b-Jc_b.transpose()*_fcDes).norm()));
//    sendMsg("Joint dynamics error = "+toString((M_a*_ddqDes+h_j-Jc_j.transpose()*_fcDes-torques).norm()));
    sendMsg("Contact constr error = "+toString((_Jc*_ddqDes+_dJcdq).norm()));
    

#ifdef  DEBUG_POSE_TASKS
    for(it=_equalityTasks.begin(); it!=_equalityTasks.end(); it++)
    {
        MinJerkPDLinkPoseTask& t = **it;
        t.getEqualityMatrix(_A_i);
        t.getEqualityVector(_b_i);
        sendMsg(t.getName()+" ||A*x-b|| =    "+toString((_A_i*_ddqDes-_b_i).norm()));
    }
#endif
    
    return true;
}

//**************************************************************************************************
bool wbiStackOfTasks::computeNullspaceProj(RobotState& robotState, Eigen::VectorRef torques)
{
    sendMsg("computeNullspaceProj");
    
    // compute ddq that is consistent with contact constraints
    SVD _Jc_svd;
    _Jc_svd.compute(_Jc, ComputeThinU|ComputeThinV);
    _ddqDes             = - svdSolveWithDamping(_Jc_svd, _dJcdq, _numericalDampingConstr);
    
    sendMsg("-Jcpinv*dJc*dq = "+toString(_ddqDes,1));
    
    // compute postural task
    int r = (_Jc_svd.singularValues().array()>PINV_TOL).count();
    MatrixRXd Nc        = MatrixRXd::Identity(_n+6,_n+6);
    Nc                  -= _Jc_svd.matrixV().leftCols(r) * _Jc_svd.matrixV().leftCols(r).transpose();
    MatrixRXd NcSTpinvD = pinvDampedEigen(Nc.rightCols(_n), _numericalDampingDyn);
    _ddqDes             += NcSTpinvD.transpose()*(_ddq_jPosture - _ddqDes.tail(_n));
    
    // compute torques
    torques             = NcSTpinvD * (_M*_ddqDes + _h);

    
//
//    sendMsg("ddqDes1 + NcSTpinvD^T*(_ddq_jPosture - ddqDes1) = "+toString(ddqDes,1));
//    sendMsg("(_M*ddqDes + _h)= "+toString((_M*ddqDes + _h),1));
    VectorXd torques_np6 = VectorXd::Zero(_n+6);
    torques_np6.tail(_n) = torques;
    _fcDes = svdSolveTransposeWithDamping(_Jc_svd, _M*_ddqDes+_h-torques_np6, _numericalDampingDyn);
    sendMsg("Dynamics error           = "+toString((Nc*(_M*_ddqDes+_h-torques_np6)).norm()));
    sendMsg("Contact constr error     = "+toString((_Jc*_ddqDes+_dJcdq).norm()));
    sendMsg("Rank of Jc:                "+toString(r));
    
    return true;
}

//**************************************************************************************************
bool wbiStackOfTasks::computeComPosture(RobotState& robotState, Eigen::VectorRef torques)
{
    sendMsg("computeComPosture");
    
    START_PROFILING(PROFILE_FORCE_QP_MOMENTUM);
    {
        // @todo Check if possible to avoid this matrix-matrix multiplication
        _qpData_f.H   = _X.topRows<3>().transpose()*_X.topRows<3>() + _numericalDampingTask*_W;
        _qpData_f.g   = -_X.topRows<3>().transpose()*_momentumDes.head<3>();
        
//        sendMsg("H diag: "+toString(_qpData_f.H.diagonal(),1));
//        sendMsg("f weights: "+toString(1e3*_fWeights,1));
//        sendMsg("X*N_X = "+toString((_X*_N_X).maxCoeff())+" "+toString((_X*_N_X).minCoeff()));
    }
    STOP_PROFILING(PROFILE_FORCE_QP_MOMENTUM);

    //    sendMsg("momentumDes "+toString(_momentumDes,1));
    //    cout<< "QP gradient "<<toString(_qpData_f.g,1)<<endl;
    double res;
    START_PROFILING(PROFILE_FORCE_QP);
    {
        res = solve_quadprog(_qpData_f.H, _qpData_f.g, _qpData_f.CE.transpose(), _qpData_f.ce0,
                             - _qpData_f.CI.transpose(), _qpData_f.ci0, _fcDes,
                             _qpData_f.activeSet, _qpData_f.activeSetSize);
    }
    STOP_PROFILING(PROFILE_FORCE_QP);
    if(res == std::numeric_limits<double>::infinity())
        return false;
    
    if(_qpData_f.activeSetSize>0)
        sendMsg("Active constraints: "+toString(_qpData_f.activeSet.head(_qpData_f.activeSetSize).transpose()));
    sendMsg("Linear momentum error  = "+toString((_X.topRows<3>()*_fcDes - _momentumDes.head<3>()).norm()));
    
    //*********************************
    // COMPUTE DESIRED ACCELERATIONS
    //*********************************
    
    // Compute ddq and f0 (i.e. y) that respect dynamics and the contact constraints
    MatrixRXd D = MatrixRXd::Zero(6+_k, _n+6+_k);
    MatrixRXd Dpinv(_n+6+_k, _k+6), DpinvD(_n+6+_k, _k+6);
    VectorXd d(6+_k);
    
    _X_svd.compute(_X.topRows<3>(), ComputeThinU|ComputeThinV);
    int r = (_X_svd.singularValues().array()>PINV_TOL).count();
    _N_X.setIdentity();
    _N_X -= _X_svd.matrixV().leftCols(r)*_X_svd.matrixV().leftCols(r).transpose();
    
    D.topLeftCorner(6, _n+6)        = M_u;
    D.bottomLeftCorner(_k, _n+6)    = _Jc;
    D.topRightCorner(6, _k)         = -Jc_b.transpose()*_N_X;
    d.head<6>()                     = Jc_b.transpose()*_fcDes - h_b;
    d.tail(_k)                      = -_dJcdq;
    pinvDampTrunc(D, PINV_TOL, _numericalDampingDyn, Dpinv, DpinvD);
    VectorXd y                      = DpinvD*d;
    
    START_PROFILING(PROFILE_DDQ_POSTURE_TASK);
    {
        MatrixRXd N_D           = MatrixRXd::Identity(_n+6+_k,_n+6+_k) - Dpinv*D;
        MatrixRXd Jp_NDpinvD    = pinvDampedEigen(N_D.middleRows(6,_n), _numericalDampingTask);
        y                       += Jp_NDpinvD*(_ddq_jPosture - y.segment(6, _n));
    }
    STOP_PROFILING(PROFILE_DDQ_POSTURE_TASK);
    
    _ddqDes     = y.head(_n+6);
    _fcDes     += _N_X*y.tail(_k);
    torques     = M_a*_ddqDes + h_j - Jc_j.transpose()*_fcDes;
    
    sendMsg("fcDes              = "+toString(_fcDes,1));
    //    sendMsg("ddq_jDes     = "+toString(_ddq_jDes,1));
    //    sendMsg("ddq_jPosture        = "+jointToString(_ddq_jPosture,1));
    sendMsg("Base dynamics error  = "+toString((M_u*_ddqDes+h_b-Jc_b.transpose()*_fcDes).norm()));
    sendMsg("Joint dynamics error = "+toString((M_a*_ddqDes+h_j-Jc_j.transpose()*_fcDes-torques).norm()));
    sendMsg("Contact constr error = "+toString((_Jc*_ddqDes+_dJcdq).norm()));
    
    return true;
}

//**************************************************************************************************
bool wbiStackOfTasks::computeMomentumPosture(RobotState& robotState, Eigen::VectorRef torques)
{
    sendMsg("computeMomentumPosture");
    
    // compute desired contact forces
    _fcDes = pinvDampedEigen(_X, _numericalDampingTask) * _momentumDes;
    
    // compute ddq that are consistent with base dynamics and contact constraints
    MatrixRXd D = MatrixRXd::Zero(6+_k,_n+6);
    MatrixRXd Dpinv(_n+6,_k+6), DpinvD(_n+6,_k+6);
    VectorXd d(6+_k);
    D.topRows<6>()      = M_u;
    D.bottomRows(_k)    = _Jc;
    d.head<6>()         = Jc_b.transpose()*_fcDes - h_b;
    d.tail(_k)          = -_dJcdq;
    pinvDampTrunc(D, PINV_TOL, _numericalDampingConstr, Dpinv, DpinvD);
    _ddqDes             = DpinvD*d;
    
    // postural task
    MatrixRXd N_D       = MatrixRXd::Identity(_n+6,_n+6) - Dpinv*D;
    MatrixRXd SN_DpinvD = pinvDampedEigen(N_D.bottomRows(_n), _numericalDampingConstr);
    _ddqDes             += SN_DpinvD*(_ddq_jPosture - _ddqDes.tail(_n));
    torques             = M_a*_ddqDes + h_j - Jc_j.transpose()*_fcDes;
    
//    sendMsg("ddqDes             = "+jointToString(_ddqDes,1));
//    sendMsg("DEBUG ddqDes       = "+jointToString(ddqDes,1));
//    sendMsg("DEBUG ddqDes1      = "+jointToString(ddqDes1,1));
//    sendMsg("DEBUG ddqDesErr    = "+toString((ddqDes-_ddqDes).norm()));
//    sendMsg("fcDes              = "+toString(_fcDes,1));
//    sendMsg("DEBUG fcDes        = "+toString(fcDes2,1));
//    sendMsg("DEBUG fcDesErr     = "+toString((fcDes2-_fcDes).norm()));
    
    sendMsg("Base dynamics error  = "+toString((M_u*_ddqDes+h_b-Jc_b.transpose()*_fcDes).norm()));
    sendMsg("Joint dynamics error = "+toString((M_a*_ddqDes+h_j-Jc_j.transpose()*_fcDes-torques).norm()));
    sendMsg("Contact constr error = "+toString((_Jc*_ddqDes+_dJcdq).norm()));
    sendMsg("tau (mom+post)       = "+jointToString(torques,1));

    return true;
}


#ifdef DEBUG_FORWARD_DYNAMICS
//**************************************************************************************************
void wbiStackOfTasks::constrainedForwardDynamics(Eigen::Vector3d& g, Eigen::VectorConst torques,
                                                 wbi::Frame &xBase, Eigen::VectorRef qj,
                                                 Eigen::VectorRef dq, Eigen::VectorRef ddq)
{
    // compute constraint matrix and vector: A*ddq = b
//    for(int i=0; i<_constrainedLinkIds.size(); i++)
//    {
//        _robot->computeJacobian(qj.data(), xBase, _constrainedLinkIds[i], _A.row(i*6).data());
//        _robot->computeDJdq(qj.data(), xBase, dq.tail(_n).data(), dq.data(), _constrainedLinkIds[i], &_b[i*6]);
//    }
//    _b *= -1.0;
    
    // compute mass matrix and generalized bias forces
//    _robot->computeMassMatrix(qj.data(), xBase, _M.data());
//    _robot->computeGeneralizedBiasForces(qj.data(), xBase, dq.tail(_n).data(), dq.data(), g.data(), _h.data());
    
    // compute constraint solution: ddqBar = - Jc^+ * dJc * dq
    _tau_np6.setZero(_n+6);
    
    _Jc_svd.compute(_Jc, ComputeFullU | ComputeFullV);
    _ddqBar = -1.0*svdSolveWithDamping(_Jc_svd, _dJcdq, _numericalDampingConstr);
    
    // compute base of null space of constraint Jacobian
    int r = (_Jc_svd.singularValues().array()>PINV_TOL).count();
    _Zc = _Jc_svd.matrixV().rightCols(_n+6-r);
    
    // compute constrained accelerations ddq_c = (Z^T*M*Z)^{-1}*Z^T*(S^T*tau - h - M*ddqBar)
    _ZMZ = _Zc.transpose()*_M*_Zc;
    _ZMZ_chol.compute(_ZMZ);
    _tau_np6.tail(_n) = torques;
    _ddq_c = _Zc.transpose()*(_tau_np6 - _h - _M*_ddqBar);
    _ZMZ_chol.solveInPlace(_ddq_c);
    // compute joint accelerations
    ddq = _ddqBar + _Zc*_ddq_c;
    
//    getLogger().sendMsg("A*ddq-b = "+toString((_A*ddq-_b).norm()), MSG_STREAM_INFO);
//    getLogger().sendMsg("ddqBar = "+jointToString(ddqBar,2), MSG_STREAM_INFO);
//    getLogger().sendMsg("ddq_c  = "+toString(_ddq_c,2), MSG_STREAM_INFO);
//    getLogger().sendMsg("ddq_c RHS = "+toString(_Zc.transpose()*(_tau_np6 - _h - _M*ddqBar),2), MSG_STREAM_INFO);
//    getLogger().sendMsg("Nc*(M*ddq+h-S^T*tau) = "+toString((_Zc.transpose()*(_M*ddq+_h-_tau_np6)).norm()), MSG_STREAM_INFO);
//    getLogger().sendMsg("Jc^T*f = "+toString(_M*ddq+_h-_tau_np6, 1), MSG_STREAM_INFO);
//    getLogger().sendMsg("ddq integration: "+jointToString(WBR_RAD2DEG*ddq,1), MSG_STREAM_INFO);
//    getLogger().sendMsg("Jc:\n"+toString(_Jc,1), MSG_STREAM_INFO);
}
#endif

//**************************************************************************************************
void wbiStackOfTasks::updateNullspace(JacobiSVD<MatrixRXd>& svd)
{
    int r = (svd.singularValues().array()>PINV_TOL).count();
    if(_useNullspaceBase)
        _Z *= svd.matrixV().rightCols(svd.cols()-r);
    else
        _Z -= svd.matrixV().leftCols(r)*svd.matrixV().leftCols(r).transpose();
}

//**************************************************************************************************
void wbiStackOfTasks::computeMb_inverse()
{
    // @todo Improve this computation exploting structure of Mb
    _Mb_llt.compute(M_b);
    _Mb_inv.setIdentity(6,6);
    _Mb_llt.solveInPlace(_Mb_inv);
}

//**************************************************************************************************
void wbiStackOfTasks::init(RobotState& robotState)
{
    _postureTask->init(robotState);
    _momentumTask->init(robotState);
    for(list<ContactConstraint*>::iterator it=_constraints.begin(); it!=_constraints.end(); it++)
        (*it)->init(robotState);
    for(list<MinJerkPDLinkPoseTask*>::iterator it=_equalityTasks.begin(); it!=_equalityTasks.end(); it++)
        (*it)->init(robotState);
        
}

//**************************************************************************************************
void wbiStackOfTasks::addConstraint(ContactConstraint& constraint)
{
    _constraints.push_back(&constraint);
    _k = 0;
    int n_in=0;
    for(list<ContactConstraint*>::iterator it=_constraints.begin(); it!=_constraints.end(); it++)
    {
        _k   += (*it)->getSize();
        n_in += (*it)->getNumberOfInequalities();
    }
    _X.setZero(6,_k);
    _X_svd = SVD(6, _k, ComputeThinU|ComputeThinV);
    _N_X.setZero(_k,_k);
    _fWeights.setZero(_k);
    _W.setZero(_k,_k);
    _Jc.setZero(_k,_n+6);
    _dJcdq.setZero(_k);
    _fcDes.setZero(_k);
    _qpData_f.H.setZero(_k,_k);
    _qpData_f.g.setZero(_k);
    _qpData_f.CI.setZero(n_in, _k);
    _qpData_f.ci0.setZero(n_in);
    _qpData_f.activeSet = VectorXi::Constant(n_in, -1);
}

//**************************************************************************************************
void wbiStackOfTasks::linkParameterToVariable(ParamTypeId paramType, ParamHelperServer* paramHelper, int paramId)
{
    switch(paramType)
    {
        case USE_NULLSPACE_BASE:
            _useNullspaceBase_paramId = paramId;
            paramHelper->linkParam(paramId, &_useNullspaceBase);
            paramHelper->registerParamValueChangedCallback(paramId, this);
            break;
        case DYN_NUM_DAMP:
            paramHelper->linkParam(paramId, &_numericalDampingDyn);
            break;
        case CONSTR_NUM_DAMP:
            paramHelper->linkParam(paramId, &_numericalDampingConstr);
            break;
        case TASK_NUM_DAMP:
            paramHelper->linkParam(paramId, &_numericalDampingTask);
            break;
        case CTRL_ALG:
            paramHelper->linkParam(paramId, &_ctrlAlg);
            break;
        default:
            cout<< "[wbiStackOfTasks::linkParameterToVariable] Trying to link a parameter that is not managed\n";
    }
}

//**************************************************************************************************
void wbiStackOfTasks::parameterUpdated(const ParamProxyInterface* pp)
{
    if(pp->id==_useNullspaceBase_paramId)
    {
        useNullspaceBase(_useNullspaceBase==1);
        return;
    }
    cout<< "[wbiStackOfTasks::parameterUpdated] Callback for a parameter that is not managed"
        << pp->name<< endl;
}
