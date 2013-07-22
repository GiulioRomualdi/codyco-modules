/*
 * Copyright (C) 2013  CoDyCo Consortium
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
 *
 * Authors: Serena Ivaldi, Andrea Del Prete, Marco Randazzo
 * email: serena.ivaldi@isir.upmc.fr - andrea.delprete@iit.it - marco.randazzo@iit.it
 */

#ifndef WBI_H
#define WBI_H

#include <vector>
#include <map>
#include <string>


/*
 * THIS CODE IS UNDER DEVELOPMENT!
 * We assume the robot is divided into subparts, which we call "body parts" (e.g. left arm, right leg, torso).
 * Each body part has an unique integer identifier.
 * In each body part there exists a unique local identifier associated to any object (e.g. joint, sensor, motor) 
 * that belongs to that body part.
 * Each object also has a unique global identifier, which defines how the objects are serialized at whole-body level.
 */


namespace wbi
{
    
    
    enum ControlMode { CTRL_MODE_OPEN_LOOP, CTRL_MODE_POS, CTRL_MODE_TORQUE, CTRL_MODE_VEL};
    
    // iterate over all body parts of the specified jointIds
#define FOR_ALL_BODY_PARTS_OF(itBp, jIds)   for (LocalIdList::const_iterator itBp=jIds.begin(); itBp!=jIds.end(); itBp++)
    // iterate over all body parts of the specified jointIds
#define FOR_ALL_BODY_PARTS_OF_NC(itBp, jIds)   for (LocalIdList::iterator itBp=jIds.begin(); itBp!=jIds.end(); itBp++)
    // iterate over all joints of the specified body part
#define FOR_ALL_JOINTS(itBp, itJ)           for(vector<int>::const_iterator itJ=itBp->second.begin(); itJ!=itBp->second.end(); itJ++)
    // as before, but it uses a nonconst iterator
#define FOR_ALL_JOINTS_NC(itBp, itJ)        for(vector<int>::iterator itJ=itBp->second.begin(); itJ!=itBp->second.end(); itJ++)
    // iterate over all joints of all body parts of the specified jointIds
#define FOR_ALL_OF(itBp, itJ, jIds)         FOR_ALL_BODY_PARTS_OF(itBp, jIds) FOR_ALL_JOINTS(itBp, itJ)
    // iterate over all joints of all body parts of the specified jointIds
#define FOR_ALL_OF_NC(itBp, itJ, jIds)      FOR_ALL_BODY_PARTS_OF_NC(itBp, jIds) FOR_ALL_JOINTS_NC(itBp, itJ)

    /**
     * Identifier composed by a body part identifier and a relative id that identifies the object locally (on the body part).
     */
    class LocalId
    {
    public:
        // body part id
        int bodyPart;
        // local id
        int index;
        // description
        std::string description;
        
        LocalId(): bodyPart(0), index(0) {}
        LocalId(int _bp, unsigned int _j): bodyPart(_bp), index(_j) {}
        LocalId(int _bp, unsigned int _j, std::string &_desc): bodyPart(_bp), index(_j), description(_desc) {}
        bool operator==(const LocalId &other) const { return (bodyPart==other.bodyPart && index==other.index); }
    };
    
    
    /**
     * List of identifiers, that is a map from body part identifiers to lists of numbers.
     */
    class LocalIdList : public std::map< int, std::vector<int> >
    {
    public:
        LocalIdList();
        LocalIdList(int bp, std::vector<int>);
        
        /** Convert a local id to a global id */
        virtual int localToGlobalId(const LocalId &i);

        /** Convert a global id to a local id */
        virtual LocalId globalToLocalId(int globalId);
        
        /** Remove the specified joint from the list */
        virtual bool removeId(const LocalId &i);
        
        /** Add the specified id to the list.
         * @param i id to add
         * @return true if the id has been added, false if it was already present
         */
        virtual bool addId(const LocalId &i);
        
        /** Add the specified ids to the list.
         * @param i id list to add
         * @return the number of ids added to the list
         */
        virtual int addIdList(const LocalIdList &i);
        
        /** Get the number of ids in this list */
        virtual unsigned int size();
        
        /* Check whether the specified body part is present in this list. */
        virtual bool containsBodyPart(int bp){ return !(find(bp)==end()); }
        
        /* Check whether the specified id is present in this list. */        
        virtual bool containsId(const LocalId &i);
    };
    
    
    /*
     * Interface for reading all the sensors of the robot.
     */
    class iWholeBodySensors
    {
    public:
        virtual bool init() = 0;
        
        virtual bool removeJoint(const LocalId &j) = 0;
        virtual bool addJoint(const LocalId &j) = 0;
        virtual int addJoints(const LocalIdList &j) = 0;
        
        virtual int getDoFs() = 0;
        
        virtual bool readEncoders(double *q, double *stamps=0, bool wait=true) = 0;
        virtual bool readPwm(double *pwm, double *stamps=0, bool wait=true) = 0;
        virtual bool readInertial(double *inertial, double *stamps=0, bool wait=true) = 0;
        virtual bool readFTsensors(double *ftSens, double *stamps=0, bool wait=true) = 0;
    };
    
    /**
      * Interface to access the estimates of the state of the robot.
      */
    class iWholeBodyStates
    {
    public:
        virtual bool init() = 0;
        virtual int getDoFs() = 0;
        virtual bool removeJoint(const LocalId &j) = 0;
        virtual bool addJoint(const LocalId &j) = 0;
        virtual int addJoints(const LocalIdList &j) = 0;
        
        virtual bool getQ(double *q, double time=-1.0, bool wait=false) = 0;
        virtual bool getDq(double *dq, double time=-1.0, bool wait=false) = 0;
        virtual bool getDqMotors(double *dqM, double time=-1.0, bool wait=false) = 0;
        virtual bool getD2q(double *d2q, double time=-1.0, bool wait=false) = 0;
        virtual bool getPwm(double *pwm, double time=-1.0, bool wait=false) = 0;
        virtual bool getInertial(double *inertial, double time=-1.0, bool wait=false) = 0;
        virtual bool getFTsensors(double *ftSens, double time=-1.0, bool wait=false) = 0;
        virtual bool getTorques(double *tau, double time=-1.0, bool wait=false) = 0;
        //virtual bool getExternalForces(double *fExt, double time=-1.0, bool wait=false) = 0;
    };
    
// TODO: Should we include base pose in joint angle vector?
    
    /**
      * Interface to the kinematic/dynamic model of the robot.
      */
    class iWholeBodyModel
    {
    public:
        virtual bool init() = 0;
        virtual int getDoFs() = 0;
        virtual bool removeJoint(const LocalId &j) = 0;
        virtual bool addJoint(const LocalId &j) = 0;
        virtual int addJoints(const LocalIdList &j) = 0;
        virtual bool getJointLimits(double *qMin, double *qMax, int joint=-1) = 0;
        
        /** Compute rototranslation matrix from root reference frame to reference frame associated to the specified link.
          * @param q Joint angles
          * @param xBase Pose of the robot base, 3 values for position and 4 values for orientation
          * @param linkId Id of the link that is the target of the rototranslation
          * @param H Output 4x4 rototranslation matrix (stored by rows)
          * @return True if the operation succeeded, false otherwise (invalid input parameters) */
        virtual bool computeH(double *q, double *xBase, int linkId, double *H) = 0;
        
        /** Compute the Jacobian of the specified point of the robot.
          * @param q Joint angles
          * @param xBase Pose of the robot base, 3 values for position and 4 values for orientation
          * @param linkId Id of the link
          * @param J Output 6xN Jacobian matrix (stored by rows), where N=number of joints
          * @param pos 3d position of the point expressed w.r.t the link reference frame
          * @return True if the operation succeeded, false otherwise (invalid input parameters) */
        virtual bool computeJacobian(double *q, double *xBase, int linkId, double *J, double *pos=0) = 0;
        
        /** Given a point on the robot, compute the product between the time derivative of its 
          * Jacobian and the joint velocity vector.
          * @param q Joint angles
          * @param xBase Pose of the robot base, 3 values for position and 4 values for orientation
          * @param dq Joint velocities
          * @param linkId Id of the link
          * @param dJdq Output 6-dim vector containing the product dJ*dq 
          * @param pos 3d position of the point expressed w.r.t the link reference frame
          * @return True if the operation succeeded, false otherwise (invalid input parameters) */
        virtual bool computeDJdq(double *q, double *xB, double *dq, double *dxB, int linkId, double *dJdq, double *pos=0) = 0;
        
        /** Compute the forward kinematics of the specified joint.
          * @param q Joint angles
          * @param xB Pose of the robot base, 3 values for position and 4 values for orientation
          * @param linkId Id of the link
          * @param x Output 7-dim pose vector (3 for pos, 4 for angle-axis orientation)
          * @return True if operation succeeded, false otherwise */
        virtual bool forwardKinematics(double *q, double *xB, int linkId, double *x) = 0;
        
        /** Compute the inverse dynamics.
          * @param q Joint angles
          * @param xB Pose of the robot base, 3 values for position and 4 values for orientation
          * @param dq Joint velocities
          * @param dxB Velocity of the robot base, 3 values for linear velocity and 3 values for angular velocity
          * @param ddq Joint accelerations
          * @param ddxB Acceleration of the robot base, 3 values for linear acceleration and 3 values for angular acceleration
          * @param tau Output joint torques
         * @return True if operation succeeded, false otherwise */
        virtual bool inverseDynamics(double *q, double *xB, double *dq, double *dxB, double *ddq, double *ddxB, double *tau) = 0;

        /** Compute the direct dynamics.
         * @param q Joint angles
         * @param xB Pose of the robot base, 3 values for position and 4 values for orientation
         * @param dq Joint velocities
         * @param dxB Velocity of the robot base, 3 values for linear velocity and 3 values for angular velocity
         * @param M Output NxN mass matrix, with N=number of joints
         * @param h Output N-dim vector containing all generalized bias forces (gravity+Coriolis+centrifugal) 
         * @return True if operation succeeded, false otherwise */
        virtual bool directDynamics(double *q, double *xB, double *dq, double *dxB, double *M, double *h) = 0;
    };
    
    /**
      * Interface to the actuators of the robot.
      */
    class iWholeBodyActuators
    {
    public:
        virtual bool init() = 0;
        virtual int getDoFs() = 0;
        virtual bool removeJoint(const LocalId &j) = 0;
        virtual bool addJoint(const LocalId &j) = 0;
        virtual int addJoints(const LocalIdList &j) = 0;
        
        /** Set the control mode of the specified joint(s).
          * @param controlMode Id of the control mode
          * @param joint Joint number, if negative, all joints are considered
          * @return True if operation succeeded, false otherwise */
        virtual bool setControlMode(int controlMode, int joint=-1) = 0;
        virtual bool setTorqueRef(double *taud, int joint=-1) = 0;
        virtual bool setPosRef(double *qd, int joint=-1) = 0;
        virtual bool setVelRef(double *dqd, int joint=-1) = 0;
        virtual bool setPwmRef(double *pwmd, int joint=-1) = 0;
    };
    
    /**
      * Interface to state estimations, kinematic/dynamic model and actuators of the robot.
      */
    class wholeBodyInterface: public iWholeBodyStates, public iWholeBodyModel, public iWholeBodyActuators
    {
    public:
        virtual bool init() = 0;
        virtual int getDoFs() = 0;
        virtual bool removeJoint(const LocalId &j) = 0;
        virtual bool addJoint(const LocalId &j) = 0;
        virtual int addJoints(const LocalIdList &j) = 0;
    };
    
    
} // end namespace

#endif
