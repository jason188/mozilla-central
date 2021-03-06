/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef __nsXULTreeAccessible_h__
#define __nsXULTreeAccessible_h__

#include "nsITreeBoxObject.h"
#include "nsITreeView.h"
#include "nsITreeColumns.h"
#include "nsXULListboxAccessible.h"

/*
 * A class the represents the XUL Tree widget.
 */
const PRUint32 kMaxTreeColumns = 100;
const PRUint32 kDefaultTreeCacheSize = 256;

/**
 * Accessible class for XUL tree element.
 */

class nsXULTreeAccessible : public nsAccessibleWrap
{
public:
  using nsAccessible::GetChildAt;

  nsXULTreeAccessible(nsIContent* aContent, DocAccessible* aDoc);

  // nsISupports and cycle collection
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(nsXULTreeAccessible,
                                           nsAccessible)

  // nsAccessNode
  virtual void Shutdown();

  // nsAccessible
  virtual void Value(nsString& aValue);
  virtual mozilla::a11y::role NativeRole();
  virtual PRUint64 NativeState();
  virtual nsAccessible* ChildAtPoint(PRInt32 aX, PRInt32 aY,
                                     EWhichChildAtPoint aWhichChild);

  virtual nsAccessible* GetChildAt(PRUint32 aIndex);
  virtual PRUint32 ChildCount() const;

  // SelectAccessible
  virtual bool IsSelect();
  virtual already_AddRefed<nsIArray> SelectedItems();
  virtual PRUint32 SelectedItemCount();
  virtual nsAccessible* GetSelectedItem(PRUint32 aIndex);
  virtual bool IsItemSelected(PRUint32 aIndex);
  virtual bool AddItemToSelection(PRUint32 aIndex);
  virtual bool RemoveItemFromSelection(PRUint32 aIndex);
  virtual bool SelectAll();
  virtual bool UnselectAll();

  // Widgets
  virtual bool IsWidget() const;
  virtual bool IsActiveWidget() const;
  virtual bool AreItemsOperable() const;
  virtual nsAccessible* CurrentItem();
  virtual void SetCurrentItem(nsAccessible* aItem);

  virtual nsAccessible* ContainerWidget() const;

  // nsXULTreeAccessible

  /**
   * Return tree item accessible at the givem row. If accessible doesn't exist
   * in the cache then create and cache it.
   *
   * @param aRow         [in] the given row index
   */
  nsAccessible* GetTreeItemAccessible(PRInt32 aRow);

  /**
   * Invalidates the number of cached treeitem accessibles.
   *
   * @param aRow    [in] row index the invalidation starts from
   * @param aCount  [in] the number of treeitem accessibles to invalidate,
   *                 the number sign specifies whether rows have been
   *                 inserted (plus) or removed (minus)
   */
  void InvalidateCache(PRInt32 aRow, PRInt32 aCount);

  /**
   * Fires name change events for invalidated area of tree.
   *
   * @param aStartRow  [in] row index invalidation starts from
   * @param aEndRow    [in] row index invalidation ends, -1 means last row index
   * @param aStartCol  [in] column index invalidation starts from
   * @param aEndCol    [in] column index invalidation ends, -1 mens last column
   *                    index
   */
  void TreeViewInvalidated(PRInt32 aStartRow, PRInt32 aEndRow,
                           PRInt32 aStartCol, PRInt32 aEndCol);

  /**
   * Invalidates children created for previous tree view.
   */
  void TreeViewChanged(nsITreeView* aView);

protected:
  /**
   * Creates tree item accessible for the given row index.
   */
  virtual already_AddRefed<nsAccessible> CreateTreeItemAccessible(PRInt32 aRow);

  nsCOMPtr<nsITreeBoxObject> mTree;
  nsITreeView* mTreeView;
  nsAccessibleHashtable mAccessibleCache;
};

/**
 * Base class for tree item accessibles.
 */

#define NS_XULTREEITEMBASEACCESSIBLE_IMPL_CID         \
{  /* 1ab79ae7-766a-443c-940b-b1e6b0831dfc */         \
  0x1ab79ae7,                                         \
  0x766a,                                             \
  0x443c,                                             \
  { 0x94, 0x0b, 0xb1, 0xe6, 0xb0, 0x83, 0x1d, 0xfc }  \
}

class nsXULTreeItemAccessibleBase : public nsAccessibleWrap
{
public:
  using nsAccessible::GetParent;

  nsXULTreeItemAccessibleBase(nsIContent* aContent, DocAccessible* aDoc,
                              nsAccessible* aParent, nsITreeBoxObject* aTree,
                              nsITreeView* aTreeView, PRInt32 aRow);

  // nsISupports and cycle collection
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(nsXULTreeItemAccessibleBase,
                                           nsAccessibleWrap)

  // nsIAccessible
  NS_IMETHOD GetBounds(PRInt32 *aX, PRInt32 *aY,
                       PRInt32 *aWidth, PRInt32 *aHeight);

  NS_IMETHOD SetSelected(bool aSelect); 
  NS_IMETHOD TakeFocus();

  NS_IMETHOD GroupPosition(PRInt32 *aGroupLevel,
                           PRInt32 *aSimilarItemsInGroup,
                           PRInt32 *aPositionInGroup);

  NS_IMETHOD GetActionName(PRUint8 aIndex, nsAString& aName);
  NS_IMETHOD DoAction(PRUint8 aIndex);

  // nsAccessNode
  virtual void Shutdown();
  virtual bool IsPrimaryForNode() const;

  // nsAccessible
  virtual PRUint64 NativeState();
  virtual PRInt32 IndexInParent() const;
  virtual Relation RelationByType(PRUint32 aType);
  virtual nsAccessible* FocusedChild();

  // ActionAccessible
  virtual PRUint8 ActionCount();

  // Widgets
  virtual nsAccessible* ContainerWidget() const;

  // nsXULTreeItemAccessibleBase
  NS_DECLARE_STATIC_IID_ACCESSOR(NS_XULTREEITEMBASEACCESSIBLE_IMPL_CID)

  /**
   * Return row index associated with the accessible.
   */
  PRInt32 GetRowIndex() const { return mRow; }

  /**
   * Return cell accessible for the given column. If XUL tree accessible is not
   * accessible table then return null.
   */
  virtual nsAccessible* GetCellAccessible(nsITreeColumn *aColumn)
    { return nsnull; }

  /**
   * Proccess row invalidation. Used to fires name change events.
   */
  virtual void RowInvalidated(PRInt32 aStartColIdx, PRInt32 aEndColIdx) = 0;

protected:
  enum { eAction_Click = 0, eAction_Expand = 1 };

  // nsAccessible
  virtual void DispatchClickEvent(nsIContent *aContent, PRUint32 aActionIndex);
  virtual nsAccessible* GetSiblingAtOffset(PRInt32 aOffset,
                                           nsresult *aError = nsnull) const;

  // nsXULTreeItemAccessibleBase

  /**
   * Return true if the tree item accessible is expandable (contains subrows).
   */
  bool IsExpandable();

  /**
   * Return name for cell at the given column.
   */
  void GetCellName(nsITreeColumn* aColumn, nsAString& aName);

  nsCOMPtr<nsITreeBoxObject> mTree;
  nsITreeView* mTreeView;
  PRInt32 mRow;
};

NS_DEFINE_STATIC_IID_ACCESSOR(nsXULTreeItemAccessibleBase,
                              NS_XULTREEITEMBASEACCESSIBLE_IMPL_CID)


/**
 * Accessible class for items for XUL tree.
 */
class nsXULTreeItemAccessible : public nsXULTreeItemAccessibleBase
{
public:
  nsXULTreeItemAccessible(nsIContent* aContent, DocAccessible* aDoc,
                          nsAccessible* aParent, nsITreeBoxObject* aTree,
                          nsITreeView* aTreeView, PRInt32 aRow);

  // nsISupports and cycle collection
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(nsXULTreeItemAccessible,
                                           nsXULTreeItemAccessibleBase)

  // nsAccessNode
  virtual bool Init();
  virtual void Shutdown();

  // nsAccessible
  virtual mozilla::a11y::ENameValueFlag Name(nsString& aName);
  virtual mozilla::a11y::role NativeRole();

  // nsXULTreeItemAccessibleBase
  virtual void RowInvalidated(PRInt32 aStartColIdx, PRInt32 aEndColIdx);

protected:

  // nsAccessible
  virtual void CacheChildren();

  // nsXULTreeItemAccessible
  nsCOMPtr<nsITreeColumn> mColumn;
  nsString mCachedName;
};


/**
 * Accessible class for columns element of XUL tree.
 */
class nsXULTreeColumnsAccessible : public nsXULColumnsAccessible
{
public:
  nsXULTreeColumnsAccessible(nsIContent* aContent, DocAccessible* aDoc);

protected:

  // nsAccessible
  virtual nsAccessible* GetSiblingAtOffset(PRInt32 aOffset,
                                           nsresult *aError = nsnull) const;
};

////////////////////////////////////////////////////////////////////////////////
// nsAccessible downcasting method

inline nsXULTreeAccessible*
nsAccessible::AsXULTree()
{
  return IsXULTree() ?
    static_cast<nsXULTreeAccessible*>(this) : nsnull;
}

#endif
