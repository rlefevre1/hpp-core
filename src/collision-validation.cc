//
// Copyright (c) 2014 CNRS
// Authors: Florent Lamiraux
//
// This file is part of hpp-core
// hpp-core is free software: you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// hpp-core is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.  You should have
// received a copy of the GNU Lesser General Public License along with
// hpp-core  If not, see
// <http://www.gnu.org/licenses/>.

#include <hpp/core/collision-validation.hh>

#include <hpp/fcl/collision.h>

#include <pinocchio/multibody/geometry.hpp>

#include <hpp/pinocchio/body.hh>
#include <hpp/pinocchio/device.hh>
#include <hpp/pinocchio/collision-object.hh>
#include <hpp/pinocchio/configuration.hh>

#include <hpp/core/relative-motion.hh>
#include <hpp/core/collision-validation-report.hh>

namespace hpp {
  namespace core {
    namespace {
      inline std::size_t collide (const CollisionPairs_t::const_iterator& _colPair,
          const fcl::CollisionRequest& req, fcl::CollisionResult& res) {
        res.clear();
	hppDout (info, "checking collision between " << _colPair->first->name ()
		 << " and " << _colPair->second->name () << "." << std::endl);
        return fcl::collide (
                    _colPair->first ->fcl (),
                    _colPair->second->fcl (),
                    req, res);
      }

      inline bool collide (const CollisionPairs_t& pairs,
         const fcl::CollisionRequest& req, fcl::CollisionResult& res,
         CollisionPairs_t::const_iterator& _col) {
        for (_col = pairs.begin (); _col != pairs.end (); ++_col)
          if (collide (_col, req, res) != 0)
            return true;
        return false;
      }

      inline CollisionPair_t makeCollisionPair (const DevicePtr_t& d, const se3::CollisionPair& p)
      {
        CollisionObjectConstPtr_t o1 (new pinocchio::CollisionObject(d, p.first));
        CollisionObjectConstPtr_t o2 (new pinocchio::CollisionObject(d, p.second));
        return CollisionPair_t (o1, o2);
      }
    }

    CollisionValidationPtr_t CollisionValidation::create
    (const DevicePtr_t& robot)
    {
      CollisionValidation* ptr = new CollisionValidation (robot);
      return CollisionValidationPtr_t (ptr);
    }

    bool CollisionValidation::validate (const Configuration_t& config,
                                        ValidationReportPtr_t& validationReport)
    {
      robot_->currentConfiguration (config);
      robot_->computeForwardKinematics ();
      robot_->updateGeometryPlacements ();

      fcl::CollisionResult collisionResult;
      CollisionPairs_t::const_iterator _col;
      if (collide (collisionPairs_, collisionRequest_, collisionResult, _col)
          ||
          ( checkParameterized_ &&
            collide (parameterizedPairs_, collisionRequest_, collisionResult, _col)
          )) {
        CollisionValidationReportPtr_t report (new CollisionValidationReport);
        report->object1 = _col->first;
        report->object2 = _col->second;
        report->result = collisionResult;
        validationReport = report;
        return false;
      }
      return true;
    }

    void CollisionValidation::addObstacle (const CollisionObjectConstPtr_t& object)
    {
      const JointVector_t& jv = robot_->getJointVector ();
      for (JointVector_t::const_iterator it = jv.begin (); it != jv.end ();
          ++it) {
        JointPtr_t joint = JointPtr_t (new Joint(**it));
        BodyPtr_t body = joint->linkedBody ();
        if (body) {
          const ObjectVector_t& bodyObjects = body->innerObjects ();
          for (ObjectVector_t::const_iterator itInner = bodyObjects.begin ();
              itInner != bodyObjects.end (); ++itInner) {
            // TODO: check the objects are not in same joint
            collisionPairs_.push_back (CollisionPair_t (*itInner, object));
          }
        }
      }
    }

    void CollisionValidation::removeObstacleFromJoint
    (const JointPtr_t& joint, const CollisionObjectConstPtr_t& obstacle)
    {
      BodyPtr_t body = joint->linkedBody ();
      if (body) {
        const ObjectVector_t& bodyObjects = body->innerObjects ();
        for (ObjectVector_t::const_iterator itInner = bodyObjects.begin ();
            itInner != bodyObjects.end (); ++itInner) {
          CollisionPair_t colPair (*itInner, obstacle);
          std::size_t nbDelPairs = 0;
          CollisionPairs_t::iterator _collisionPair (collisionPairs_.begin());
          while ( (_collisionPair = std::find (_collisionPair, collisionPairs_.end(), colPair))
              != collisionPairs_.end()) {
            _collisionPair = collisionPairs_.erase (_collisionPair);
            ++nbDelPairs;
          }
          if (nbDelPairs == 0) {
            std::ostringstream oss;
            oss << "CollisionValidation::removeObstacleFromJoint: obstacle \""
                << obstacle->name () <<
                "\" is not registered as obstacle for joint \"" << joint->name ()
                << "\".";
            throw std::runtime_error (oss.str ());
          } else if (nbDelPairs >= 2) {
            hppDout (error, "obstacle "<< obstacle->name () <<
                     " was registered " << nbDelPairs
                     << " times as obstacle for joint " << joint->name ()
                     << ".");
          }
        }
      }
    }

    void CollisionValidation::filterCollisionPairs (const RelativeMotion::matrix_type& matrix)
    {
      // Loop over collision pairs and remove disabled ones.
      CollisionPairs_t::iterator _colPair = collisionPairs_.begin ();
      se3::JointIndex j1, j2;
      fcl::CollisionResult unused;
      while (_colPair != collisionPairs_.end ()) {
        j1 = _colPair->first ->jointIndex();
        j2 = _colPair->second->jointIndex();

        switch (matrix(j1, j2)) {
          case RelativeMotion::Parameterized:
              hppDout(info, "Parameterized collision pairs between "
                  << _colPair->first ->name() << " and "
                  << _colPair->second->name());
	      hppDout(info, "j1=" << j1 << ", j2=" << j2);
              parameterizedPairs_.push_back (*_colPair);
              _colPair = collisionPairs_.erase (_colPair);
              break;
          case RelativeMotion::Constrained:
              hppDout(info, "Disabling collision between "
                  << _colPair->first ->name() << " and "
                  << _colPair->second->name());
              if (collide (_colPair, collisionRequest_, unused) != 0) {
                hppDout(warning, "Disabling collision detection between two "
                    "body in collision.");
              }
              disabledPairs_.push_back (*_colPair);
              _colPair = collisionPairs_.erase (_colPair);
              break;
          case RelativeMotion::Unconstrained: ++_colPair; break;
          default:
            hppDout (warning, "RelativeMotionType not understood");
            ++_colPair;
            break;
        }
      }
    }

    CollisionValidation::CollisionValidation (const DevicePtr_t& robot) :
      collisionRequest_(1, false, false, 1, false, true, fcl::GST_INDEP),
      robot_ (robot),
      parameterizedPairs_(), disabledPairs_(),
      checkParameterized_(false)
    {
      const se3::GeometryModel& model = robot->geomModel();
      const se3::GeometryData & data  = robot->geomData();

      for (std::size_t i = 0; i < model.collisionPairs.size(); ++i)
        if (data.activeCollisionPairs[i])
          collisionPairs_.push_back(
              makeCollisionPair(robot, model.collisionPairs[i])
              );
    }

  } // namespace core
} // namespace hpp
